//========= Copyright Valve Corporation, All rights reserved. ============//
//
// NativeActivity entry point for the Android/VR port.
//
// Bridges Android's native_app_glue lifecycle into the engine's existing
// Android launcher entry point (LauncherMainAndroid, see main.cpp). This
// replaces the old SDL Java-Activity/JNI bridge (ValveActivity2) with a
// plain android.app.NativeActivity, since VR runtimes (OpenXR) expect to
// drive the native window/session lifecycle directly rather than through
// an SDL-owned Activity.
//
// This is scaffolding: it gets the engine process started once a native
// window exists. OpenXR session creation and the render loop are wired up
// separately once a Vulkan renderer backend exists.
//
//===========================================================================//

#include <android/log.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>
#include <jni.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

#include "openxr_bootstrap.h"
#include "vr_xr_gles.h"
#include "appframework/ilaunchermgr.h"

extern "C" int LauncherMainAndroid( int argc, char **argv ); // launcher/android/main.cpp
extern ILauncherMgr *g_pLauncherMgr; // appframework/sdlmgr.cpp

namespace
{
	struct android_app *g_pAndroidApp;
	pthread_t g_engineThread;
	bool g_bEngineStarted;
	ANativeWindow *g_pAcquiredWindow;
	XrInstance g_xrInstance = XR_NULL_HANDLE;
	XrSystemId g_xrSystemId = XR_NULL_SYSTEM_ID;
	bool g_bVrSessionReady;

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

	bool g_bOpenXrInitDone;

	void *EngineThreadMain( void * )
	{
		LauncherMainAndroid( 0, NULL );
		return NULL;
	}

	// The engine thread must not start until BOTH the native window exists and
	// InitOpenXR() has finished publishing the per-eye render size, because
	// SetLauncherArgs() (launcher/android/main.cpp) reads HL2VR_EYE_WIDTH/HEIGHT
	// to build the -w/-h arguments that set the stereo backbuffer size.
	//
	// These two are genuinely concurrent: xrCreateInstance binds to the OpenXR
	// runtime broker over Binder, which pumps this thread's ALooper
	// re-entrantly, so APP_CMD_INIT_WINDOW gets dispatched *during*
	// InitOpenXR() rather than after it. Starting the engine from the command
	// handler therefore beat the env vars by ~1s and the engine silently fell
	// back to a 640x480 backbuffer.
	void StartEngineIfReady( struct android_app *app )
	{
		// Gate on g_pAcquiredWindow, NOT app->window. The activity is
		// routinely paused/stopped and its window torn down (APP_CMD_
		// TERM_WINDOW) while InitOpenXR is still running - xrCreateInstance
		// takes ~1s binding the runtime broker - so by the time we get here
		// app->window is often already NULL again. g_pAcquiredWindow holds a
		// strong ANativeWindow_acquire() reference taken when the window
		// first appeared, which is exactly what keeps it valid across that,
		// and is what HL2VR_ANATIVE_WINDOW already points at.
		if ( g_bEngineStarted || !g_bOpenXrInitDone || g_pAcquiredWindow == NULL )
			return;

		g_bEngineStarted = true;
		setenv( "APP_DATA_PATH", app->activity->internalDataPath, 1 );
		// GetBaseDirectory() (launcher.cpp) reads this, not APP_DATA_PATH.
		// Game content lives under the app's external storage (internal
		// storage is too small and raw POSIX I/O can't reach /sdcard
		// directly under scoped storage) - already pushed there in an
		// earlier session and still present on this device.
		{
			char gamePath[512];
			snprintf( gamePath, sizeof( gamePath ), "%s/hl2vr_content", app->activity->externalDataPath );
			setenv( "VALVE_GAME_PATH", gamePath, 1 );
			__android_log_print( ANDROID_LOG_INFO, "hl2vr", "VALVE_GAME_PATH=%s", gamePath );
		}
		SetAppLibPathEnv( app );
		pthread_create( &g_engineThread, NULL, EngineThreadMain, NULL );
	}

	void HandleAppCmd( struct android_app *app, int32_t cmd )
	{
		switch ( cmd )
		{
		case APP_CMD_INIT_WINDOW:
			if ( app->window != NULL )
			{
				// android_native_app_glue hands out app->window as a raw,
				// unowned pointer - nothing stops Android's own window
				// management from releasing the underlying ANativeWindow
				// (an android::RefBase-derived object) once its refcount
				// drops, even though the pointer value itself doesn't
				// change. ANativeWindow_acquire() takes a real strong
				// reference so the object stays valid for as long as the
				// renderer (or anything else) might use it.
				if ( g_pAcquiredWindow != app->window )
				{
					if ( g_pAcquiredWindow )
						ANativeWindow_release( g_pAcquiredWindow );
					ANativeWindow_acquire( app->window );
					g_pAcquiredWindow = app->window;
				}

				char nativeWindowHex[32];
				snprintf( nativeWindowHex, sizeof( nativeWindowHex ), "%llx", (unsigned long long)(uintptr_t)app->window );
				setenv( "HL2VR_ANATIVE_WINDOW", nativeWindowHex, 1 );
			}

			// The native window is ready. The engine runs on its own thread so
			// this one can keep pumping the Android event loop - the engine's
			// main loop isn't written to interleave with ALooper polling.
			// May be a no-op here if InitOpenXR() hasn't finished yet; that
			// path starts it instead (see StartEngineIfReady).
			StartEngineIfReady( app );
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

	// Loader init / instance / system query. Doesn't require a native
	// window - the session (which does) is created lazily below, once the
	// engine thread has brought up its EGL context (see g_bVrSessionReady).
	InitOpenXR( app, &g_xrInstance, &g_xrSystemId );

	// Only now are HL2VR_EYE_WIDTH/HEIGHT published, so the engine may start.
	// If the window already arrived while InitOpenXR was running (it usually
	// does - see StartEngineIfReady), this is what actually launches it.
	g_bOpenXrInitDone = true;
	StartEngineIfReady( app );

	while ( true )
	{
		int events;
		struct android_poll_source *source;

		// Block unless there's actually something to poll for.
		//
		// The only reason to wake up on a timer is the EGL-ready check below,
		// which can't be satisfied until the engine thread exists - so before
		// that, and again once the session is up, just sleep until Android
		// has an event for us.
		//
		// This used to use a 0ms (non-blocking) timeout before the engine
		// started, i.e. a busy spin. That was survivable in an -O0 build but
		// pins a core in an optimized one, and starved delivery of
		// APP_CMD_INIT_WINDOW badly enough that app->window stayed NULL and
		// the engine never started at all.
		int timeoutMs = ( g_bEngineStarted && !g_bVrSessionReady ) ? 50 : -1;
		while ( ALooper_pollOnce( timeoutMs, NULL, &events, (void **)&source ) >= 0 )
		{
			if ( source != NULL )
				source->process( app, source );

			if ( app->destroyRequested != 0 )
				return;
		}

		// CSDLMgr (appframework/sdlmgr.cpp) creates its EGL context lazily,
		// on the engine thread, the first time materialsystem connects -
		// not necessarily by the time this loop starts. Poll for it instead
		// of trying to synchronize the two threads directly.
		if ( g_xrInstance != XR_NULL_HANDLE && !g_bVrSessionReady && g_pLauncherMgr != NULL
			&& g_pLauncherMgr->GetEglDisplay() != NULL )
		{
			g_bVrSessionReady = VRXR_Init( g_xrInstance, g_xrSystemId );
			if ( !g_bVrSessionReady )
				g_xrInstance = XR_NULL_HANDLE; // don't keep retrying a session that failed to build
		}
	}
}
