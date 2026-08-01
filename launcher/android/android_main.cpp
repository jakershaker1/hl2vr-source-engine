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
#include <android_native_app_glue.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

#include "openxr_bootstrap.h"

extern "C" int LauncherMainAndroid( int argc, char **argv ); // launcher/android/main.cpp

namespace
{
	struct android_app *g_pAndroidApp;
	pthread_t g_engineThread;
	bool g_bEngineStarted;

	void *EngineThreadMain( void * )
	{
		LauncherMainAndroid( 0, NULL );
		return NULL;
	}

	void HandleAppCmd( struct android_app *app, int32_t cmd )
	{
		switch ( cmd )
		{
		case APP_CMD_INIT_WINDOW:
			// The native window is ready. Start the engine on its own thread so
			// this thread can keep pumping the Android event loop - the engine's
			// main loop isn't written to interleave with ALooper polling.
			if ( app->window != NULL && !g_bEngineStarted )
			{
				g_bEngineStarted = true;
				setenv( "APP_DATA_PATH", app->activity->internalDataPath, 1 );
				pthread_create( &g_engineThread, NULL, EngineThreadMain, NULL );
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

	// Loader init / instance / system query only - no session yet (needs a
	// Vulkan device, see task #6/#7). Doesn't require a native window.
	InitOpenXR( app );

	while ( true )
	{
		int events;
		struct android_poll_source *source;

		int timeoutMs = g_bEngineStarted ? -1 : 0;
		while ( ALooper_pollOnce( timeoutMs, NULL, &events, (void **)&source ) >= 0 )
		{
			if ( source != NULL )
				source->process( app, source );

			if ( app->destroyRequested != 0 )
				return;
		}
	}
}
