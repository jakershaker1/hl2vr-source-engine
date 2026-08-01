//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Minimal OpenXR bootstrap for Android. See openxr_bootstrap.h.
//
//===========================================================================//

#include "openxr_bootstrap.h"

#include <android/log.h>
#include <android_native_app_glue.h>
#include "tier1/strtools.h"

#define XR_USE_PLATFORM_ANDROID
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

bool InitOpenXR( struct android_app *app )
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

	const char *enabledExtensions[] = { XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME };

	XrInstanceCreateInfoAndroidKHR androidCreateInfo = { XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR };
	androidCreateInfo.applicationVM = app->activity->vm;
	androidCreateInfo.applicationActivity = app->activity->clazz;

	XrInstanceCreateInfo createInfo = { XR_TYPE_INSTANCE_CREATE_INFO };
	createInfo.next = &androidCreateInfo;
	createInfo.enabledExtensionCount = 1;
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

	// No graphics binding yet, so no session - just tear the instance back
	// down. Real session creation happens once there's a Vulkan device to
	// hand OpenXR (task #6/#7).
	xrDestroyInstance( instance );
	return true;
}
