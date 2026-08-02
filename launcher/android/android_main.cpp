//========= Copyright Valve Corporation, All rights reserved. ============//
//
// NativeActivity entry point for the Android/VR port.
//
// Bridges Android's native_app_glue lifecycle into OpenXR + a minimal
// Vulkan presentation loop (see openxr_bootstrap.*, vr_session.*). This
// replaces the old SDL Java-Activity/JNI bridge (ValveActivity2) with a
// plain android.app.NativeActivity, since VR runtimes (OpenXR) expect to
// drive the native window/session lifecycle directly rather than through
// an SDL-owned Activity.
//
// The VR session (openxr_bootstrap/vr_session) is a standalone
// visual-validation path (clears each eye to an animated color) - it is
// not yet wired into the engine's materialsystem/shaderapi (that's tracked
// separately). The real engine entry point (LauncherMainAndroid) is also
// started here, on its own thread, so we can see how far engine init gets
// (e.g. locating HL2 game content) independent of the VR render path.
//
//===========================================================================//

#include <android/log.h>
#include <android_native_app_glue.h>
#include <jni.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "openxr_bootstrap.h"
#include "vr_session.h"

extern "C" int LauncherMainAndroid( int argc, char **argv ); // launcher/android/main.cpp

namespace
{
	struct android_app *g_pAndroidApp;
	pthread_t g_vrThread;
	pthread_t g_engineThread;
	bool g_bVrStarted;
	bool g_bEngineStarted;

	struct VrThreadArgs
	{
		XrInstance instance;
		XrSystemId systemId;
	};

	void *VrThreadEntry( void *pArg )
	{
		VrThreadArgs *args = (VrThreadArgs *)pArg;
		RunVRSession( g_pAndroidApp, args->instance, args->systemId );
		delete args;
		return NULL;
	}

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

	XrInstance instance = XR_NULL_HANDLE;
	XrSystemId systemId = XR_NULL_SYSTEM_ID;
	if ( InitOpenXR( app, &instance, &systemId ) )
	{
		VrThreadArgs *args = new VrThreadArgs{ instance, systemId };
		g_bVrStarted = true;
		pthread_create( &g_vrThread, NULL, VrThreadEntry, args );
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
