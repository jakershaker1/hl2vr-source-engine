//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Minimal OpenXR bootstrap for Android. See openxr_bootstrap.h.
//
//===========================================================================//

#include "openxr_bootstrap.h"

#include <android/log.h>
#include <android_native_app_glue.h>
#include <stdlib.h>
#include <sys/system_properties.h>
#include "tier1/strtools.h"

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <EGL/egl.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#define LOG_TAG "hl2vr.openxr"
#define LOGI( ... ) __android_log_print( ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__ )
#define LOGE( ... ) __android_log_print( ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__ )

namespace
{
	bool XrCheck( XrResult result, const char *what )
	{
		if ( XR_SUCCEEDED( result ) )
			return true;
		LOGE( "%s failed: XrResult %d", what, (int)result );
		return false;
	}
}

bool InitOpenXR( struct android_app *app, XrInstance *pInstance, XrSystemId *pSystemId )
{
	// The Android loader needs to be handed the JavaVM/Context before any
	// other OpenXR call - it uses this to find and bind the runtime's
	// broker service. This must go through xrGetInstanceProcAddr with a
	// null instance, since xrInitializeLoaderKHR is a loader-only function
	// that exists before any instance (and therefore any function table).
	PFN_xrInitializeLoaderKHR xrInitializeLoaderKHR = NULL;
	xrGetInstanceProcAddr( XR_NULL_HANDLE, "xrInitializeLoaderKHR", (PFN_xrVoidFunction *)&xrInitializeLoaderKHR );
	if ( xrInitializeLoaderKHR == NULL )
	{
		LOGE( "xrInitializeLoaderKHR not available - no OpenXR loader/runtime present" );
		return false;
	}

	XrLoaderInitInfoAndroidKHR loaderInitInfo = { XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR };
	loaderInitInfo.applicationVM = app->activity->vm;
	loaderInitInfo.applicationContext = app->activity->clazz;
	if ( !XrCheck( xrInitializeLoaderKHR( (XrLoaderInitInfoBaseHeaderKHR *)&loaderInitInfo ), "xrInitializeLoaderKHR" ) )
		return false;

	const char *enabledExtensions[] = { XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME, XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME };

	XrInstanceCreateInfoAndroidKHR androidCreateInfo = { XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR };
	androidCreateInfo.applicationVM = app->activity->vm;
	androidCreateInfo.applicationActivity = app->activity->clazz;

	XrInstanceCreateInfo createInfo = { XR_TYPE_INSTANCE_CREATE_INFO };
	createInfo.next = &androidCreateInfo;
	createInfo.enabledExtensionCount = 2;
	createInfo.enabledExtensionNames = enabledExtensions;
	V_strncpy( createInfo.applicationInfo.applicationName, "HL2VR", sizeof( createInfo.applicationInfo.applicationName ) );
	createInfo.applicationInfo.applicationVersion = 1;
	createInfo.applicationInfo.apiVersion = XR_API_VERSION_1_0;

	XrInstance instance = XR_NULL_HANDLE;
	if ( !XrCheck( xrCreateInstance( &createInfo, &instance ), "xrCreateInstance" ) )
		return false;

	XrInstanceProperties instanceProps = { XR_TYPE_INSTANCE_PROPERTIES };
	if ( XrCheck( xrGetInstanceProperties( instance, &instanceProps ), "xrGetInstanceProperties" ) )
	{
		LOGI( "OpenXR runtime: %s (version %u.%u.%u)", instanceProps.runtimeName,
			XR_VERSION_MAJOR( instanceProps.runtimeVersion ),
			XR_VERSION_MINOR( instanceProps.runtimeVersion ),
			XR_VERSION_PATCH( instanceProps.runtimeVersion ) );
	}

	XrSystemGetInfo systemGetInfo = { XR_TYPE_SYSTEM_GET_INFO };
	systemGetInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;

	XrSystemId systemId = XR_NULL_SYSTEM_ID;
	if ( !XrCheck( xrGetSystem( instance, &systemGetInfo, &systemId ), "xrGetSystem" ) )
	{
		xrDestroyInstance( instance );
		return false;
	}

	XrSystemProperties systemProps = { XR_TYPE_SYSTEM_PROPERTIES };
	if ( XrCheck( xrGetSystemProperties( instance, systemId, &systemProps ), "xrGetSystemProperties" ) )
	{
		LOGI( "OpenXR system: %s (vendorId 0x%x, maxLayers %u, orientationTracking %d, positionTracking %d)",
			systemProps.systemName, systemProps.vendorId, systemProps.graphicsProperties.maxLayerCount,
			systemProps.trackingProperties.orientationTracking, systemProps.trackingProperties.positionTracking );
	}

	// Publish the runtime's recommended per-eye render size, so the engine's
	// backbuffer can be forced to exactly two of them side by side. Consumed
	// by engine/sys_getmodes.cpp (video mode) and
	// materialsystem/shaderapidx9/shaderdevicedx8.cpp (the actual D3D
	// backbuffer allocation), plus CSDLMgr::DisplayedSize which is what
	// ISourceVirtualReality::GetViewportBounds splits per eye.
	//
	// Handed over via env vars - the same convention android_main.cpp already
	// uses for HL2VR_ANATIVE_WINDOW - because neither the engine nor
	// appframework may link OpenXR.
	//
	// It has to happen here, before the engine thread starts, because the
	// engine picks its resolution during materialsystem init, long before the
	// XrSession (and therefore the swapchains) exist. Only the instance and
	// system ID are needed to ask, so it is available this early. See
	// StartEngineIfReady in android_main.cpp for why that ordering needs
	// enforcing rather than assuming.
	//
	// Getting this wrong is not subtle: the engine otherwise falls back to a
	// tiny enumerated video mode (640x480, then 320x240) while
	// GetViewportBounds hands out 1856x2160 per-eye viewports, so the left
	// eye is clipped and the right eye lands entirely off-buffer. Each eye
	// then receives a stretched sliver - a static, per-eye world distortion
	// that looks like broken projection math even when the projection and
	// pose math are provably correct.
	{
		uint32_t viewCount = 0;
		if ( XR_SUCCEEDED( xrEnumerateViewConfigurationViews( instance, systemId,
			XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, NULL ) ) && viewCount >= 1 )
		{
			XrViewConfigurationView views[2] = { { XR_TYPE_VIEW_CONFIGURATION_VIEW }, { XR_TYPE_VIEW_CONFIGURATION_VIEW } };
			if ( viewCount > 2 )
				viewCount = 2;
			if ( XR_SUCCEEDED( xrEnumerateViewConfigurationViews( instance, systemId,
				XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, viewCount, &viewCount, views ) ) )
			{
				uint32_t eyeW = views[0].recommendedImageRectWidth;
				uint32_t eyeH = views[0].recommendedImageRectHeight;

				// Optional render-scale, tunable without a rebuild:
				//   adb shell setprop debug.hl2vr.resscale 0.7
				// then relaunch. Scales the per-eye render target while
				// keeping its aspect ratio (aspect is what has to match the
				// submitted FOV - getting it wrong shears the world), so the
				// compositor upscales to the swapchain instead of us
				// rendering native.
				//
				// NOTE: this only reduces GPU fill cost. Measured on this
				// device the app is CPU-bound (app CPU ~47ms vs GPU ~9ms at
				// full native), so expect little from it until the CPU side
				// (two full engine render passes per frame through togles'
				// D3D9->GLES translation) is addressed.
				char scaleProp[PROP_VALUE_MAX] = {};
				if ( __system_property_get( "debug.hl2vr.resscale", scaleProp ) > 0 )
				{
					float scale = (float)atof( scaleProp );
					if ( scale > 0.1f && scale < 1.0f )
					{
						// Round to a multiple of 8 - some drivers are much
						// happier with aligned render target dimensions.
						eyeW = ( (uint32_t)( eyeW * scale ) + 7 ) & ~7u;
						eyeH = ( (uint32_t)( eyeH * scale ) + 7 ) & ~7u;
						LOGI( "Applying render scale %.2f", scale );
					}
				}

				char buf[32];
				V_snprintf( buf, sizeof( buf ), "%u", eyeW );
				setenv( "HL2VR_EYE_WIDTH", buf, 1 );
				V_snprintf( buf, sizeof( buf ), "%u", eyeH );
				setenv( "HL2VR_EYE_HEIGHT", buf, 1 );
				// The 2D UI gets its own strip of the backbuffer, which
				// vr_xr_gles.cpp blits into a separate OpenXR quad layer so it
				// is never composited into either eye.
				//
				// 4:3 rather than 16:9 on purpose: HL2's main menu is laid out
				// for a 4:3 screen, and CBasePanel::PerformLayout pushes the
				// menu upwards when it does not fit the available height
				// ("idealMenuY = tall - menuTall - inset"), which slid it up
				// underneath the game logo on a short panel.
				V_snprintf( buf, sizeof( buf ), "%d", 1280 );
				setenv( "HL2VR_UI_WIDTH", buf, 1 );
				V_snprintf( buf, sizeof( buf ), "%d", 960 );
				setenv( "HL2VR_UI_HEIGHT", buf, 1 );

				LOGI( "Per-eye render size: %ux%u (runtime recommended %ux%u), UI panel 1280x960",
					eyeW, eyeH, views[0].recommendedImageRectWidth, views[0].recommendedImageRectHeight );
			}
		}
	}

	*pInstance = instance;
	*pSystemId = systemId;
	return true;
}
