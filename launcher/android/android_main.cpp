//========= Copyright Valve Corporation, All rights reserved. ============//
//
// NativeActivity entry point for the Android/VR port.
//
// Bridges Android's native_app_glue lifecycle into OpenXR (see
// openxr_bootstrap.*) and the real engine (LauncherMainAndroid). This
// replaces the old SDL Java-Activity/JNI bridge (ValveActivity2) with a
// plain android.app.NativeActivity, since VR runtimes (OpenXR) expect to
// drive the native window/session lifecycle directly rather than through
// an SDL-owned Activity.
//
// The XrInstance/XrSystemId created here are handed to the engine (which
// runs in the same process, but a different .so - materialsystem's
// shaderapivulkan module) via env vars, following this codebase's existing
// convention for passing Android-specific state across module boundaries
// (see APP_DATA_PATH/VALVE_GAME_PATH/APP_LIB_PATH below). shaderapivulkan
// owns creating the actual Vulkan device/session/swapchains and driving
// the per-frame present (see launcher/android/vr_xr_vulkan.*).
//
//===========================================================================//

#include <android/log.h>
#include <android_native_app_glue.h>
#include <inttypes.h>
#include <jni.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "openxr_bootstrap.h"

extern "C" int LauncherMainAndroid( int argc, char **argv ); // launcher/android/main.cpp

namespace
{
	struct android_app *g_pAndroidApp;
	pthread_t g_engineThread;
	bool g_bEngineStarted;

	void *EngineThreadEntry( void * )
	{
		LauncherMainAndroid( 0, NULL );
		return NULL;
	}

	// Tier1's module loader (tier1/interface.cpp Sys_LoadModule) resolves engine
	// .so's like filesystem_stdio.so via stat() against $APP_LIB_PATH, not via
	// the dynamic linker's own search path. ANativeActivity doesn't expose the
	// APK's native library directory directly, so fetch it from
	// ApplicationInfo.nativeLibraryDir over JNI.
	bool SetAppLibPathEnv( struct android_app *app )
	{
		JNIEnv *env = NULL;
		if ( app->activity->vm->AttachCurrentThread( &env, NULL ) != JNI_OK || env == NULL )
			return false;

		bool ok = false;
		jclass activityClass = env->GetObjectClass( app->activity->clazz );
		jmethodID getApplicationInfo = env->GetMethodID( activityClass, "getApplicationInfo", "()Landroid/content/pm/ApplicationInfo;" );
		jobject appInfo = env->CallObjectMethod( app->activity->clazz, getApplicationInfo );
		if ( appInfo != NULL )
		{
			jclass appInfoClass = env->GetObjectClass( appInfo );
			jfieldID nativeLibraryDirField = env->GetFieldID( appInfoClass, "nativeLibraryDir", "Ljava/lang/String;" );
			jstring nativeLibraryDir = (jstring)env->GetObjectField( appInfo, nativeLibraryDirField );
			if ( nativeLibraryDir != NULL )
			{
				const char *path = env->GetStringUTFChars( nativeLibraryDir, NULL );
				setenv( "APP_LIB_PATH", path, 1 );
				__android_log_print( ANDROID_LOG_INFO, "hl2vr", "APP_LIB_PATH=%s", path );
				env->ReleaseStringUTFChars( nativeLibraryDir, path );
				ok = true;
			}
		}

		if ( env->ExceptionCheck() )
			env->ExceptionClear();

		app->activity->vm->DetachCurrentThread();
		return ok;
	}

	void HandleAppCmd( struct android_app *app, int32_t cmd )
	{
		switch ( cmd )
		{
		case APP_CMD_INIT_WINDOW:
			if ( app->window != NULL && !g_bEngineStarted )
			{
				g_bEngineStarted = true;
				setenv( "APP_DATA_PATH", app->activity->internalDataPath, 1 );

				// GetBaseDirectory() (launcher.cpp) reads this, not APP_DATA_PATH.
				// Game content (hl2/, platform/) is pushed under the app's own
				// external files dir - raw POSIX file I/O (which the engine's
				// filesystem code uses) can't reach arbitrary /sdcard paths under
				// scoped storage, only this app-specific one.
				char gamePath[1024];
				snprintf( gamePath, sizeof( gamePath ), "%s/hl2vr_content", app->activity->externalDataPath );
				setenv( "VALVE_GAME_PATH", gamePath, 1 );
				__android_log_print( ANDROID_LOG_INFO, "hl2vr", "VALVE_GAME_PATH=%s", gamePath );

				// DXVK Native's logger writes Info+ level to a file (path from
				// DXVK_LOG_PATH) - handy for diagnosing device/init failures
				// on-device, since std::cerr doesn't reliably reach logcat.
				// internalDataPath (not external) - reliably reachable via
				// `adb shell run-as`, unlike this device's external storage.
				setenv( "DXVK_LOG_PATH", app->activity->internalDataPath, 1 );
				setenv( "DXVK_LOG_LEVEL", "debug", 1 );
				// DXVK has no sensible default WSI backend on non-Windows
				// platforms (see dxvk_native/src/wsi/wsi_platform.cpp) -
				// without this it throws during DxvkInstance construction,
				// on DXVK's own internal thread, before anything is logged.
				// "Headless" is our own custom WSI backend added to that
				// file - SDL2's real Android video backend needs JNI
				// bootstrapping via its own Java Activity class, which this
				// android_native_app_glue-based app never performs, and we
				// don't need DXVK's swapchain to be visible on screen
				// anyway (see dxvk_xr_bridge.cpp).
				setenv( "DXVK_WSI_DRIVER", "Headless", 1 );

				// VK_EXT_headless_surface isn't supported by this device's
				// Vulkan driver (confirmed on-device), so the "Headless" WSI
				// driver actually uses VK_KHR_android_surface (which is
				// supported) against this app's real ANativeWindow instead -
				// safe to share since our own OpenXR/Vulkan rendering
				// (vr_xr_vulkan.cpp) never touches the raw 2D window, only
				// OpenXR's own swapchain.
				char nativeWindowHex[32];
				snprintf( nativeWindowHex, sizeof( nativeWindowHex ), "%llx", (unsigned long long)(uintptr_t)app->window );
				setenv( "HL2VR_ANATIVE_WINDOW", nativeWindowHex, 1 );

				SetAppLibPathEnv( app );
				pthread_create( &g_engineThread, NULL, EngineThreadEntry, NULL );
			}
			break;

		case APP_CMD_DESTROY:
			__android_log_print( ANDROID_LOG_INFO, "hl2vr", "APP_CMD_DESTROY - exiting" );
			_exit( 0 );
			break;

		default:
			break;
		}
	}

	int32_t HandleInputEvent( struct android_app *app, AInputEvent *event )
	{
		// Real controller/hand input is wired up via OpenXR action sets, not
		// through Android's input event queue - nothing to do here yet.
		return 0;
	}
}

void android_main( struct android_app *app )
{
	g_pAndroidApp = app;
	app->onAppCmd = HandleAppCmd;
	app->onInputEvent = HandleInputEvent;

	// Loader init / instance / system query only. shaderapivulkan (loaded
	// later, by the engine, in a different .so) owns creating the actual
	// Vulkan device/session/swapchains from this instance - hand it over via
	// env vars, the same mechanism used for the other Android-specific state
	// below.
	XrInstance instance = XR_NULL_HANDLE;
	XrSystemId systemId = XR_NULL_SYSTEM_ID;
	if ( InitOpenXR( app, &instance, &systemId ) )
	{
		char buf[32];
		snprintf( buf, sizeof( buf ), "%" PRIxPTR, (uintptr_t)instance );
		setenv( "HL2VR_XR_INSTANCE", buf, 1 );
		snprintf( buf, sizeof( buf ), "%" PRIx64, (uint64_t)systemId );
		setenv( "HL2VR_XR_SYSTEM_ID", buf, 1 );
	}

	while ( true )
	{
		int events;
		struct android_poll_source *source;

		while ( ALooper_pollOnce( 0, NULL, &events, (void **)&source ) >= 0 )
		{
			if ( source != NULL )
				source->process( app, source );

			if ( app->destroyRequested != 0 )
				return;
		}
	}
}
