//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Minimal OpenXR + Vulkan presentation loop. See vr_session.h.
//
//===========================================================================//

#include "vr_session.h"

#include <android/log.h>
#include <android_native_app_glue.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

#define LOG_TAG "hl2vr.vr"
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

	bool VkCheck( VkResult result, const char *what )
	{
		if ( result == VK_SUCCESS )
			return true;
		LOGE( "%s failed: VkResult %d", what, (int)result );
		return false;
	}

	// xrGetVulkanInstanceExtensionsKHR/xrGetVulkanDeviceExtensionsKHR return a
	// single space-delimited string; split it into a vector the Vulkan API wants.
	std::vector<std::string> SplitExtensionString( const char *str )
	{
		std::vector<std::string> out;
		const char *p = str;
		while ( *p )
		{
			while ( *p == ' ' ) p++;
			if ( !*p ) break;
			const char *start = p;
			while ( *p && *p != ' ' ) p++;
			out.emplace_back( start, p - start );
		}
		return out;
	}

	struct VRState
	{
		XrInstance instance = XR_NULL_HANDLE;
		XrSystemId systemId = XR_NULL_SYSTEM_ID;
		XrSession session = XR_NULL_HANDLE;
		XrSpace localSpace = XR_NULL_HANDLE;
		XrSessionState sessionState = XR_SESSION_STATE_UNKNOWN;
		bool sessionRunning = false;
		bool exitRequested = false;

		VkInstance vkInstance = VK_NULL_HANDLE;
		VkPhysicalDevice vkPhysicalDevice = VK_NULL_HANDLE;
		VkDevice vkDevice = VK_NULL_HANDLE;
		uint32_t queueFamilyIndex = 0;
		VkQueue vkQueue = VK_NULL_HANDLE;
		VkCommandPool vkCommandPool = VK_NULL_HANDLE;

		std::vector<XrViewConfigurationView> viewConfigViews;

		struct Eye
		{
			XrSwapchain swapchain = XR_NULL_HANDLE;
			int64_t format = 0;
			uint32_t width = 0, height = 0;
			std::vector<VkImage> images;
			std::vector<VkCommandBuffer> commandBuffers;
		};
		std::vector<Eye> eyes;
	};

	bool CreateVulkanInstanceForXR( VRState &vr )
	{
		PFN_xrGetVulkanGraphicsRequirementsKHR xrGetVulkanGraphicsRequirementsKHR = NULL;
		PFN_xrGetVulkanInstanceExtensionsKHR xrGetVulkanInstanceExtensionsKHR = NULL;
		xrGetInstanceProcAddr( vr.instance, "xrGetVulkanGraphicsRequirementsKHR", (PFN_xrVoidFunction *)&xrGetVulkanGraphicsRequirementsKHR );
		xrGetInstanceProcAddr( vr.instance, "xrGetVulkanInstanceExtensionsKHR", (PFN_xrVoidFunction *)&xrGetVulkanInstanceExtensionsKHR );
		if ( !xrGetVulkanGraphicsRequirementsKHR || !xrGetVulkanInstanceExtensionsKHR )
		{
			LOGE( "Vulkan XR extension functions not available" );
			return false;
		}

		XrGraphicsRequirementsVulkanKHR reqs = { XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR };
		if ( !XrCheck( xrGetVulkanGraphicsRequirementsKHR( vr.instance, vr.systemId, &reqs ), "xrGetVulkanGraphicsRequirementsKHR" ) )
			return false;

		uint32_t extCount = 0;
		xrGetVulkanInstanceExtensionsKHR( vr.instance, vr.systemId, 0, &extCount, NULL );
		std::vector<char> extBuf( extCount );
		xrGetVulkanInstanceExtensionsKHR( vr.instance, vr.systemId, extCount, &extCount, extBuf.data() );
		std::vector<std::string> extNames = SplitExtensionString( extBuf.data() );
		std::vector<const char *> extPtrs;
		for ( const std::string &s : extNames )
			extPtrs.push_back( s.c_str() );

		VkApplicationInfo appInfo = {};
		appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
		appInfo.pApplicationName = "HL2VR";
		appInfo.apiVersion = VK_API_VERSION_1_1;

		VkInstanceCreateInfo createInfo = {};
		createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
		createInfo.pApplicationInfo = &appInfo;
		createInfo.enabledExtensionCount = (uint32_t)extPtrs.size();
		createInfo.ppEnabledExtensionNames = extPtrs.empty() ? NULL : extPtrs.data();

		return VkCheck( vkCreateInstance( &createInfo, NULL, &vr.vkInstance ), "vkCreateInstance" );
	}

	bool CreateVulkanDeviceForXR( VRState &vr )
	{
		PFN_xrGetVulkanGraphicsDeviceKHR xrGetVulkanGraphicsDeviceKHR = NULL;
		PFN_xrGetVulkanDeviceExtensionsKHR xrGetVulkanDeviceExtensionsKHR = NULL;
		xrGetInstanceProcAddr( vr.instance, "xrGetVulkanGraphicsDeviceKHR", (PFN_xrVoidFunction *)&xrGetVulkanGraphicsDeviceKHR );
		xrGetInstanceProcAddr( vr.instance, "xrGetVulkanDeviceExtensionsKHR", (PFN_xrVoidFunction *)&xrGetVulkanDeviceExtensionsKHR );
		if ( !xrGetVulkanGraphicsDeviceKHR || !xrGetVulkanDeviceExtensionsKHR )
		{
			LOGE( "Vulkan XR device extension functions not available" );
			return false;
		}

		if ( !XrCheck( xrGetVulkanGraphicsDeviceKHR( vr.instance, vr.systemId, vr.vkInstance, &vr.vkPhysicalDevice ), "xrGetVulkanGraphicsDeviceKHR" ) )
			return false;

		uint32_t queueFamilyCount = 0;
		vkGetPhysicalDeviceQueueFamilyProperties( vr.vkPhysicalDevice, &queueFamilyCount, NULL );
		std::vector<VkQueueFamilyProperties> queueFamilies( queueFamilyCount );
		vkGetPhysicalDeviceQueueFamilyProperties( vr.vkPhysicalDevice, &queueFamilyCount, queueFamilies.data() );

		vr.queueFamilyIndex = UINT32_MAX;
		for ( uint32_t i = 0; i < queueFamilyCount; i++ )
		{
			if ( queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT )
			{
				vr.queueFamilyIndex = i;
				break;
			}
		}
		if ( vr.queueFamilyIndex == UINT32_MAX )
		{
			LOGE( "No graphics queue family found" );
			return false;
		}

		float queuePriority = 1.0f;
		VkDeviceQueueCreateInfo queueCreateInfo = {};
		queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
		queueCreateInfo.queueFamilyIndex = vr.queueFamilyIndex;
		queueCreateInfo.queueCount = 1;
		queueCreateInfo.pQueuePriorities = &queuePriority;

		uint32_t extCount = 0;
		xrGetVulkanDeviceExtensionsKHR( vr.instance, vr.systemId, 0, &extCount, NULL );
		std::vector<char> extBuf( extCount );
		xrGetVulkanDeviceExtensionsKHR( vr.instance, vr.systemId, extCount, &extCount, extBuf.data() );
		std::vector<std::string> extNames = SplitExtensionString( extBuf.data() );
		std::vector<const char *> extPtrs;
		for ( const std::string &s : extNames )
			extPtrs.push_back( s.c_str() );

		VkDeviceCreateInfo deviceCreateInfo = {};
		deviceCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
		deviceCreateInfo.queueCreateInfoCount = 1;
		deviceCreateInfo.pQueueCreateInfos = &queueCreateInfo;
		deviceCreateInfo.enabledExtensionCount = (uint32_t)extPtrs.size();
		deviceCreateInfo.ppEnabledExtensionNames = extPtrs.empty() ? NULL : extPtrs.data();

		if ( !VkCheck( vkCreateDevice( vr.vkPhysicalDevice, &deviceCreateInfo, NULL, &vr.vkDevice ), "vkCreateDevice" ) )
			return false;

		vkGetDeviceQueue( vr.vkDevice, vr.queueFamilyIndex, 0, &vr.vkQueue );

		VkCommandPoolCreateInfo poolInfo = {};
		poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		poolInfo.queueFamilyIndex = vr.queueFamilyIndex;
		return VkCheck( vkCreateCommandPool( vr.vkDevice, &poolInfo, NULL, &vr.vkCommandPool ), "vkCreateCommandPool" );
	}

	bool CreateXrSession( VRState &vr )
	{
		XrGraphicsBindingVulkanKHR binding = { XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR };
		binding.instance = vr.vkInstance;
		binding.physicalDevice = vr.vkPhysicalDevice;
		binding.device = vr.vkDevice;
		binding.queueFamilyIndex = vr.queueFamilyIndex;
		binding.queueIndex = 0;

		XrSessionCreateInfo createInfo = { XR_TYPE_SESSION_CREATE_INFO };
		createInfo.next = &binding;
		createInfo.systemId = vr.systemId;
		if ( !XrCheck( xrCreateSession( vr.instance, &createInfo, &vr.session ), "xrCreateSession" ) )
			return false;

		XrReferenceSpaceCreateInfo spaceInfo = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
		spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
		spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
		return XrCheck( xrCreateReferenceSpace( vr.session, &spaceInfo, &vr.localSpace ), "xrCreateReferenceSpace" );
	}

	bool CreateSwapchains( VRState &vr )
	{
		uint32_t viewCount = 0;
		xrEnumerateViewConfigurationViews( vr.instance, vr.systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, NULL );
		vr.viewConfigViews.resize( viewCount, { XR_TYPE_VIEW_CONFIGURATION_VIEW } );
		if ( !XrCheck( xrEnumerateViewConfigurationViews( vr.instance, vr.systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
			viewCount, &viewCount, vr.viewConfigViews.data() ), "xrEnumerateViewConfigurationViews" ) )
			return false;

		uint32_t formatCount = 0;
		xrEnumerateSwapchainFormats( vr.session, 0, &formatCount, NULL );
		std::vector<int64_t> formats( formatCount );
		xrEnumerateSwapchainFormats( vr.session, formatCount, &formatCount, formats.data() );

		// Prefer a standard SRGB 8-bit format; fall back to whatever the
		// runtime offers first if none of our preferences are present.
		int64_t chosenFormat = formats.empty() ? 0 : formats[0];
		for ( int64_t preferred : { (int64_t)VK_FORMAT_R8G8B8A8_SRGB, (int64_t)VK_FORMAT_B8G8R8A8_SRGB } )
		{
			for ( int64_t f : formats )
			{
				if ( f == preferred ) { chosenFormat = preferred; break; }
			}
		}

		vr.eyes.resize( viewCount );
		for ( uint32_t i = 0; i < viewCount; i++ )
		{
			VRState::Eye &eye = vr.eyes[i];
			eye.format = chosenFormat;
			eye.width = vr.viewConfigViews[i].recommendedImageRectWidth;
			eye.height = vr.viewConfigViews[i].recommendedImageRectHeight;

			XrSwapchainCreateInfo swapchainInfo = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
			swapchainInfo.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
			swapchainInfo.format = eye.format;
			swapchainInfo.sampleCount = 1;
			swapchainInfo.width = eye.width;
			swapchainInfo.height = eye.height;
			swapchainInfo.faceCount = 1;
			swapchainInfo.arraySize = 1;
			swapchainInfo.mipCount = 1;
			if ( !XrCheck( xrCreateSwapchain( vr.session, &swapchainInfo, &eye.swapchain ), "xrCreateSwapchain" ) )
				return false;

			uint32_t imageCount = 0;
			xrEnumerateSwapchainImages( eye.swapchain, 0, &imageCount, NULL );
			std::vector<XrSwapchainImageVulkanKHR> xrImages( imageCount, { XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR } );
			if ( !XrCheck( xrEnumerateSwapchainImages( eye.swapchain, imageCount, &imageCount,
				(XrSwapchainImageBaseHeader *)xrImages.data() ), "xrEnumerateSwapchainImages" ) )
				return false;

			eye.images.resize( imageCount );
			for ( uint32_t j = 0; j < imageCount; j++ )
				eye.images[j] = xrImages[j].image;

			VkCommandBufferAllocateInfo allocInfo = {};
			allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
			allocInfo.commandPool = vr.vkCommandPool;
			allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
			allocInfo.commandBufferCount = imageCount;
			eye.commandBuffers.resize( imageCount );
			if ( !VkCheck( vkAllocateCommandBuffers( vr.vkDevice, &allocInfo, eye.commandBuffers.data() ), "vkAllocateCommandBuffers" ) )
				return false;
		}
		return true;
	}

	// Records and submits a command buffer that clears the given swapchain
	// image to a color, leaving it in COLOR_ATTACHMENT_OPTIMAL layout for
	// the compositor. No render pass/pipeline needed for a plain clear.
	void RenderEyeClear( VRState &vr, VkImage image, VkCommandBuffer cmd, float r, float g, float b )
	{
		VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		vkBeginCommandBuffer( cmd, &beginInfo );

		VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

		VkImageMemoryBarrier toClear = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
		toClear.srcAccessMask = 0;
		toClear.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		toClear.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		toClear.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		toClear.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		toClear.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		toClear.image = image;
		toClear.subresourceRange = range;
		vkCmdPipelineBarrier( cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &toClear );

		VkClearColorValue clearColor;
		clearColor.float32[0] = r; clearColor.float32[1] = g; clearColor.float32[2] = b; clearColor.float32[3] = 1.0f;
		vkCmdClearColorImage( cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearColor, 1, &range );

		VkImageMemoryBarrier toPresent = toClear;
		toPresent.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		toPresent.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		toPresent.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		toPresent.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		vkCmdPipelineBarrier( cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &toPresent );

		vkEndCommandBuffer( cmd );

		VkSubmitInfo submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers = &cmd;
		vkQueueSubmit( vr.vkQueue, 1, &submitInfo, VK_NULL_HANDLE );
		vkQueueWaitIdle( vr.vkQueue ); // simplest possible sync - correctness over throughput for this vertical slice
	}

	void PollXrEvents( VRState &vr )
	{
		XrEventDataBuffer event;
		while ( true )
		{
			event.type = XR_TYPE_EVENT_DATA_BUFFER;
			event.next = NULL;
			XrResult result = xrPollEvent( vr.instance, &event );
			if ( result != XR_SUCCESS )
				break;

			if ( event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED )
			{
				XrEventDataSessionStateChanged *stateEvent = (XrEventDataSessionStateChanged *)&event;
				vr.sessionState = stateEvent->state;
				LOGI( "XrSessionState -> %d", (int)vr.sessionState );

				if ( vr.sessionState == XR_SESSION_STATE_READY )
				{
					XrSessionBeginInfo beginInfo = { XR_TYPE_SESSION_BEGIN_INFO };
					beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
					if ( XrCheck( xrBeginSession( vr.session, &beginInfo ), "xrBeginSession" ) )
						vr.sessionRunning = true;
				}
				else if ( vr.sessionState == XR_SESSION_STATE_STOPPING )
				{
					xrEndSession( vr.session );
					vr.sessionRunning = false;
				}
				else if ( vr.sessionState == XR_SESSION_STATE_EXITING || vr.sessionState == XR_SESSION_STATE_LOSS_PENDING )
				{
					vr.exitRequested = true;
				}
			}
		}
	}

	void RenderFrame( VRState &vr )
	{
		XrFrameWaitInfo waitInfo = { XR_TYPE_FRAME_WAIT_INFO };
		XrFrameState frameState = { XR_TYPE_FRAME_STATE };
		if ( !XrCheck( xrWaitFrame( vr.session, &waitInfo, &frameState ), "xrWaitFrame" ) )
			return;

		XrFrameBeginInfo beginInfo = { XR_TYPE_FRAME_BEGIN_INFO };
		xrBeginFrame( vr.session, &beginInfo );

		std::vector<XrCompositionLayerProjectionView> projViews;
		XrCompositionLayerProjection projLayer = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
		const XrCompositionLayerBaseHeader *layers[1] = {};
		uint32_t layerCount = 0;

		if ( frameState.shouldRender )
		{
			uint32_t viewCount = (uint32_t)vr.eyes.size();
			std::vector<XrView> views( viewCount, { XR_TYPE_VIEW } );

			XrViewLocateInfo locateInfo = { XR_TYPE_VIEW_LOCATE_INFO };
			locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
			locateInfo.displayTime = frameState.predictedDisplayTime;
			locateInfo.space = vr.localSpace;

			XrViewState viewState = { XR_TYPE_VIEW_STATE };
			uint32_t viewCountOut = 0;
			xrLocateViews( vr.session, &locateInfo, &viewState, viewCount, &viewCountOut, views.data() );

			// Slowly cycle a color per eye so it's visually obvious this is a
			// live, updating frame rather than a static image.
			static float t = 0.0f;
			t += 0.01f;
			float r = 0.5f + 0.5f * sinf( t );
			float g = 0.5f + 0.5f * sinf( t + 2.09f );
			float b = 0.5f + 0.5f * sinf( t + 4.18f );

			projViews.resize( viewCount );
			for ( uint32_t i = 0; i < viewCount; i++ )
			{
				VRState::Eye &eye = vr.eyes[i];

				XrSwapchainImageAcquireInfo acquireInfo = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
				uint32_t imageIndex = 0;
				xrAcquireSwapchainImage( eye.swapchain, &acquireInfo, &imageIndex );

				XrSwapchainImageWaitInfo waitImgInfo = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
				waitImgInfo.timeout = XR_INFINITE_DURATION;
				xrWaitSwapchainImage( eye.swapchain, &waitImgInfo );

				RenderEyeClear( vr, eye.images[imageIndex], eye.commandBuffers[imageIndex], r, g, b );

				XrSwapchainImageReleaseInfo releaseInfo = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
				xrReleaseSwapchainImage( eye.swapchain, &releaseInfo );

				projViews[i].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
				projViews[i].pose = views[i].pose;
				projViews[i].fov = views[i].fov;
				projViews[i].subImage.swapchain = eye.swapchain;
				projViews[i].subImage.imageRect.offset = { 0, 0 };
				projViews[i].subImage.imageRect.extent = { (int32_t)eye.width, (int32_t)eye.height };
			}

			projLayer.space = vr.localSpace;
			projLayer.viewCount = viewCount;
			projLayer.views = projViews.data();
			layers[0] = (const XrCompositionLayerBaseHeader *)&projLayer;
			layerCount = 1;
		}

		XrFrameEndInfo endInfo = { XR_TYPE_FRAME_END_INFO };
		endInfo.displayTime = frameState.predictedDisplayTime;
		endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
		endInfo.layerCount = layerCount;
		endInfo.layers = layers;
		xrEndFrame( vr.session, &endInfo );
	}
}

void RunVRSession( struct android_app *app, XrInstance instance, XrSystemId systemId )
{
	VRState vr;
	vr.instance = instance;
	vr.systemId = systemId;

	bool ok = CreateVulkanInstanceForXR( vr )
		&& CreateVulkanDeviceForXR( vr )
		&& CreateXrSession( vr )
		&& CreateSwapchains( vr );

	if ( !ok )
	{
		LOGE( "VR session setup failed - aborting VR render loop" );
	}
	else
	{
		LOGI( "VR session ready: %zu eye(s), %ux%u", vr.eyes.size(), vr.eyes.empty() ? 0 : vr.eyes[0].width, vr.eyes.empty() ? 0 : vr.eyes[0].height );

		while ( !vr.exitRequested && app->destroyRequested == 0 )
		{
			PollXrEvents( vr );
			if ( vr.sessionRunning )
				RenderFrame( vr );
		}
	}

	if ( vr.session != XR_NULL_HANDLE )
	{
		if ( vr.sessionRunning )
			xrEndSession( vr.session );
		xrDestroySession( vr.session );
	}
	if ( vr.vkDevice != VK_NULL_HANDLE )
		vkDestroyDevice( vr.vkDevice, NULL );
	if ( vr.vkInstance != VK_NULL_HANDLE )
		vkDestroyInstance( vr.vkInstance, NULL );
	xrDestroyInstance( vr.instance );
}
