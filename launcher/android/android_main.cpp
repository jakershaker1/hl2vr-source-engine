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
// The VR session here is a standalone visual-validation path (clears each
// eye to an animated color) - it is not yet wired into the engine's
// materialsystem/shaderapi, which is a much larger follow-up. The real
// engine entry point (LauncherMainAndroid) is defined here but not called
// yet for that reason.
//
//===========================================================================//

#include <android/log.h>
#include <android_native_app_glue.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

#include "openxr_bootstrap.h"
#include "vr_session.h"

extern "C" int LauncherMainAndroid( int argc, char **argv ); // launcher/android/main.cpp

namespace
{
	struct android_app *g_pAndroidApp;
	pthread_t g_vrThread;
	bool g_bVrStarted;

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

	void HandleAppCmd( struct android_app *app, int32_t cmd )
	{
		switch ( cmd )
		{
		case APP_CMD_INIT_WINDOW:
			if ( app->window != NULL )
				setenv( "APP_DATA_PATH", app->activity->internalDataPath, 1 );
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
