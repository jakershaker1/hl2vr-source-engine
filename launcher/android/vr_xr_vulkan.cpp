//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Reusable OpenXR + Vulkan device/session/swapchain plumbing. See
// vr_xr_vulkan.h.
//
//===========================================================================//

#include "vr_xr_vulkan.h"
#include "basic_shaders_spv.h"

#include <android/log.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <unistd.h>
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
		bool ready = false;

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
			std::vector<VkImageView> imageViews;
			std::vector<VkFramebuffer> framebuffers;
			std::vector<VkCommandBuffer> commandBuffers;
		};
		std::vector<Eye> eyes;

		// Depth buffer for eye 0's render pass - world geometry is drawn in
		// whatever order the engine issues it, not sorted or occlusion
		// culled, so without real depth testing overlapping/crossing
		// surfaces z-fight per pixel every frame (looked like "a bunch of
		// white lines" rather than solid geometry). Only eye 0 renders for
		// real (eye 1 gets a copy - see VRXR_EndSceneAndPresent), so one
		// depth buffer is enough.
		VkFormat depthFormat = VK_FORMAT_UNDEFINED;
		VkImage depthImage = VK_NULL_HANDLE;
		VkDeviceMemory depthImageMemory = VK_NULL_HANDLE;
		VkImageView depthImageView = VK_NULL_HANDLE;

		// Fixed pipeline for all draws - one hand-written unlit textured shader,
		// used regardless of what shader/material the engine actually asked for.
		VkRenderPass renderPass = VK_NULL_HANDLE;
		VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
		VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
		VkPipeline pipeline = VK_NULL_HANDLE;
		VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
		VkSampler defaultSampler = VK_NULL_HANDLE;

		// One descriptor set per distinct texture view seen so far this run;
		// small enough in practice to just linear-search.
		std::vector<std::pair<VkImageView, VkDescriptorSet>> descriptorCache;

		// Current in-flight scene, if any.
		bool frameOpen = false;
		uint32_t eyeImageIndex[2] = { 0, 0 };
		VkCommandBuffer sceneCmdBuffer = VK_NULL_HANDLE;
		XrFrameState frameState = { XR_TYPE_FRAME_STATE };
		std::vector<XrView> currentViews;

		std::vector<std::pair<VkBuffer, VkDeviceMemory>> deferredDestroy;
	};

	VRState g_Vr;

	uint32_t FindMemoryType( uint32_t typeBits, VkMemoryPropertyFlags properties )
	{
		VkPhysicalDeviceMemoryProperties memProps;
		vkGetPhysicalDeviceMemoryProperties( g_Vr.vkPhysicalDevice, &memProps );
		for ( uint32_t i = 0; i < memProps.memoryTypeCount; i++ )
		{
			if ( ( typeBits & ( 1u << i ) ) && ( memProps.memoryTypes[i].propertyFlags & properties ) == properties )
				return i;
		}
		return UINT32_MAX;
	}

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

		VkPhysicalDeviceFeatures features = {};
		VkDeviceCreateInfo deviceCreateInfo = {};
		deviceCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
		deviceCreateInfo.queueCreateInfoCount = 1;
		deviceCreateInfo.pQueueCreateInfos = &queueCreateInfo;
		deviceCreateInfo.enabledExtensionCount = (uint32_t)extPtrs.size();
		deviceCreateInfo.ppEnabledExtensionNames = extPtrs.empty() ? NULL : extPtrs.data();
		deviceCreateInfo.pEnabledFeatures = &features;

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
			swapchainInfo.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT
				| XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
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
			eye.imageViews.resize( imageCount );
			for ( uint32_t j = 0; j < imageCount; j++ )
			{
				eye.images[j] = xrImages[j].image;

				VkImageViewCreateInfo viewInfo = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
				viewInfo.image = eye.images[j];
				viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
				viewInfo.format = (VkFormat)eye.format;
				viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
				if ( !VkCheck( vkCreateImageView( vr.vkDevice, &viewInfo, NULL, &eye.imageViews[j] ), "vkCreateImageView (swapchain)" ) )
					return false;
			}

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

	bool CreateDepthBuffer( VRState &vr )
	{
		VkFormat candidates[3] = { VK_FORMAT_D32_SFLOAT, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D16_UNORM };
		for ( VkFormat fmt : candidates )
		{
			VkFormatProperties props;
			vkGetPhysicalDeviceFormatProperties( vr.vkPhysicalDevice, fmt, &props );
			if ( props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT )
			{
				vr.depthFormat = fmt;
				break;
			}
		}
		if ( vr.depthFormat == VK_FORMAT_UNDEFINED )
		{
			LOGE( "No supported depth format found" );
			return false;
		}

		if ( !VRXR_CreateImage2D( vr.eyes[0].width, vr.eyes[0].height, vr.depthFormat, VK_IMAGE_TILING_OPTIMAL,
			VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &vr.depthImage, &vr.depthImageMemory ) )
			return false;

		VkImageAspectFlags aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
		if ( vr.depthFormat == VK_FORMAT_D24_UNORM_S8_UINT )
			aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;

		VkImageViewCreateInfo viewInfo = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
		viewInfo.image = vr.depthImage;
		viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		viewInfo.format = vr.depthFormat;
		viewInfo.subresourceRange = { aspect, 0, 1, 0, 1 };
		return VkCheck( vkCreateImageView( vr.vkDevice, &viewInfo, NULL, &vr.depthImageView ), "vkCreateImageView (depth)" );
	}

	bool CreateRenderPass( VRState &vr )
	{
		VkAttachmentDescription colorAttachment = {};
		colorAttachment.format = (VkFormat)vr.eyes[0].format;
		colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
		colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		colorAttachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

		VkAttachmentDescription depthAttachment = {};
		depthAttachment.format = vr.depthFormat;
		depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
		depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

		VkAttachmentDescription attachments[2] = { colorAttachment, depthAttachment };

		VkAttachmentReference colorRef = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
		VkAttachmentReference depthRef = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };

		VkSubpassDescription subpass = {};
		subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		subpass.colorAttachmentCount = 1;
		subpass.pColorAttachments = &colorRef;
		subpass.pDepthStencilAttachment = &depthRef;

		VkSubpassDependency dependency = {};
		dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
		dependency.dstSubpass = 0;
		dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
		dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
		dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

		VkRenderPassCreateInfo rpInfo = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
		rpInfo.attachmentCount = 2;
		rpInfo.pAttachments = attachments;
		rpInfo.subpassCount = 1;
		rpInfo.pSubpasses = &subpass;
		rpInfo.dependencyCount = 1;
		rpInfo.pDependencies = &dependency;

		return VkCheck( vkCreateRenderPass( vr.vkDevice, &rpInfo, NULL, &vr.renderPass ), "vkCreateRenderPass" );
	}

	bool CreateFramebuffers( VRState &vr )
	{
		// Only eye 0 is ever actually rendered into via this render pass
		// (eye 1 gets a plain image copy of eye 0's result - see
		// VRXR_EndSceneAndPresent), so it's the only one that needs
		// framebuffers, and the only one that needs the depth attachment.
		VRState::Eye &eye = vr.eyes[0];
		eye.framebuffers.resize( eye.imageViews.size() );
		for ( size_t j = 0; j < eye.imageViews.size(); j++ )
		{
			VkImageView attachments[2] = { eye.imageViews[j], vr.depthImageView };

			VkFramebufferCreateInfo fbInfo = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
			fbInfo.renderPass = vr.renderPass;
			fbInfo.attachmentCount = 2;
			fbInfo.pAttachments = attachments;
			fbInfo.width = eye.width;
			fbInfo.height = eye.height;
			fbInfo.layers = 1;
			if ( !VkCheck( vkCreateFramebuffer( vr.vkDevice, &fbInfo, NULL, &eye.framebuffers[j] ), "vkCreateFramebuffer" ) )
				return false;
		}
		return true;
	}

	bool CreateDescriptorStuff( VRState &vr )
	{
		VkDescriptorSetLayoutBinding binding = {};
		binding.binding = 0;
		binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		binding.descriptorCount = 1;
		binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

		VkDescriptorSetLayoutCreateInfo layoutInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
		layoutInfo.bindingCount = 1;
		layoutInfo.pBindings = &binding;
		if ( !VkCheck( vkCreateDescriptorSetLayout( vr.vkDevice, &layoutInfo, NULL, &vr.descriptorSetLayout ), "vkCreateDescriptorSetLayout" ) )
			return false;

		VkDescriptorPoolSize poolSize = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4096 };
		VkDescriptorPoolCreateInfo poolInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
		poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
		poolInfo.maxSets = 4096;
		poolInfo.poolSizeCount = 1;
		poolInfo.pPoolSizes = &poolSize;
		if ( !VkCheck( vkCreateDescriptorPool( vr.vkDevice, &poolInfo, NULL, &vr.descriptorPool ), "vkCreateDescriptorPool" ) )
			return false;

		VkSamplerCreateInfo samplerInfo = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
		samplerInfo.magFilter = VK_FILTER_LINEAR;
		samplerInfo.minFilter = VK_FILTER_LINEAR;
		samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
		samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		samplerInfo.maxLod = 0.25f; // our textures are single-mip
		return VkCheck( vkCreateSampler( vr.vkDevice, &samplerInfo, NULL, &vr.defaultSampler ), "vkCreateSampler" );
	}

	VkShaderModule CreateShaderModule( VRState &vr, const unsigned int *pCode, size_t codeWords )
	{
		VkShaderModuleCreateInfo info = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
		info.codeSize = codeWords * sizeof( unsigned int );
		info.pCode = pCode;
		VkShaderModule module = VK_NULL_HANDLE;
		VkCheck( vkCreateShaderModule( vr.vkDevice, &info, NULL, &module ), "vkCreateShaderModule" );
		return module;
	}

	bool CreatePipeline( VRState &vr )
	{
		VkPushConstantRange pushConstant = {};
		pushConstant.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
		pushConstant.offset = 0;
		pushConstant.size = 16 * sizeof( float ); // mat4 mvp

		VkPipelineLayoutCreateInfo layoutInfo = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
		layoutInfo.setLayoutCount = 1;
		layoutInfo.pSetLayouts = &vr.descriptorSetLayout;
		layoutInfo.pushConstantRangeCount = 1;
		layoutInfo.pPushConstantRanges = &pushConstant;
		if ( !VkCheck( vkCreatePipelineLayout( vr.vkDevice, &layoutInfo, NULL, &vr.pipelineLayout ), "vkCreatePipelineLayout" ) )
			return false;

		VkShaderModule vertModule = CreateShaderModule( vr, g_BasicVertSpv, sizeof( g_BasicVertSpv ) / sizeof( unsigned int ) );
		VkShaderModule fragModule = CreateShaderModule( vr, g_BasicFragSpv, sizeof( g_BasicFragSpv ) / sizeof( unsigned int ) );
		if ( vertModule == VK_NULL_HANDLE || fragModule == VK_NULL_HANDLE )
			return false;

		VkPipelineShaderStageCreateInfo stages[2] = {};
		stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
		stages[0].module = vertModule;
		stages[0].pName = "main";
		stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
		stages[1].module = fragModule;
		stages[1].pName = "main";

		// Fixed vertex layout matching CMeshVulkan in shaderapivulkan.cpp:
		// interleaved [ position(3f), texcoord0(2f) ], stride 20 bytes.
		VkVertexInputBindingDescription binding = { 0, 20, VK_VERTEX_INPUT_RATE_VERTEX };
		VkVertexInputAttributeDescription attrs[2] = {
			{ 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 },
			{ 1, 0, VK_FORMAT_R32G32_SFLOAT, 12 },
		};
		VkPipelineVertexInputStateCreateInfo vertexInput = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
		vertexInput.vertexBindingDescriptionCount = 1;
		vertexInput.pVertexBindingDescriptions = &binding;
		vertexInput.vertexAttributeDescriptionCount = 2;
		vertexInput.pVertexAttributeDescriptions = attrs;

		VkPipelineInputAssemblyStateCreateInfo inputAssembly = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
		inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

		VkPipelineViewportStateCreateInfo viewportState = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
		viewportState.viewportCount = 1;
		viewportState.scissorCount = 1;

		VkPipelineRasterizationStateCreateInfo raster = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
		raster.polygonMode = VK_POLYGON_MODE_FILL;
		raster.cullMode = VK_CULL_MODE_NONE; // unknown winding across arbitrary Source geometry - skip culling for now
		raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
		raster.lineWidth = 1.0f;

		VkPipelineMultisampleStateCreateInfo multisample = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
		multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

		VkPipelineDepthStencilStateCreateInfo depthStencil = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
		depthStencil.depthTestEnable = VK_TRUE;
		depthStencil.depthWriteEnable = VK_TRUE;
		depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;

		VkPipelineColorBlendAttachmentState blendAttachment = {};
		blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
		VkPipelineColorBlendStateCreateInfo colorBlend = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
		colorBlend.attachmentCount = 1;
		colorBlend.pAttachments = &blendAttachment;

		VkDynamicState dynStates[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
		VkPipelineDynamicStateCreateInfo dynamicState = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
		dynamicState.dynamicStateCount = 2;
		dynamicState.pDynamicStates = dynStates;

		VkGraphicsPipelineCreateInfo pipelineInfo = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
		pipelineInfo.stageCount = 2;
		pipelineInfo.pStages = stages;
		pipelineInfo.pVertexInputState = &vertexInput;
		pipelineInfo.pInputAssemblyState = &inputAssembly;
		pipelineInfo.pViewportState = &viewportState;
		pipelineInfo.pRasterizationState = &raster;
		pipelineInfo.pMultisampleState = &multisample;
		pipelineInfo.pDepthStencilState = &depthStencil;
		pipelineInfo.pColorBlendState = &colorBlend;
		pipelineInfo.pDynamicState = &dynamicState;
		pipelineInfo.layout = vr.pipelineLayout;
		pipelineInfo.renderPass = vr.renderPass;
		pipelineInfo.subpass = 0;

		bool ok = VkCheck( vkCreateGraphicsPipelines( vr.vkDevice, VK_NULL_HANDLE, 1, &pipelineInfo, NULL, &vr.pipeline ), "vkCreateGraphicsPipelines" );

		vkDestroyShaderModule( vr.vkDevice, vertModule, NULL );
		vkDestroyShaderModule( vr.vkDevice, fragModule, NULL );
		return ok;
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

	VkDescriptorSet GetOrCreateDescriptorSet( VRState &vr, VkImageView view, VkSampler sampler )
	{
		for ( auto &entry : vr.descriptorCache )
		{
			if ( entry.first == view )
				return entry.second;
		}

		VkDescriptorSetAllocateInfo allocInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
		allocInfo.descriptorPool = vr.descriptorPool;
		allocInfo.descriptorSetCount = 1;
		allocInfo.pSetLayouts = &vr.descriptorSetLayout;

		VkDescriptorSet set = VK_NULL_HANDLE;
		if ( vkAllocateDescriptorSets( vr.vkDevice, &allocInfo, &set ) != VK_SUCCESS )
		{
			LOGE( "vkAllocateDescriptorSets failed (descriptor pool exhausted?)" );
			return VK_NULL_HANDLE;
		}

		VkDescriptorImageInfo imageInfo = { sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
		VkWriteDescriptorSet write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
		write.dstSet = set;
		write.dstBinding = 0;
		write.descriptorCount = 1;
		write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		write.pImageInfo = &imageInfo;
		vkUpdateDescriptorSets( vr.vkDevice, 1, &write, 0, NULL );

		vr.descriptorCache.emplace_back( view, set );
		return set;
	}
}

bool VRXR_Init( XrInstance instance, XrSystemId systemId )
{
	g_Vr.instance = instance;
	g_Vr.systemId = systemId;

	g_Vr.ready = CreateVulkanInstanceForXR( g_Vr )
		&& CreateVulkanDeviceForXR( g_Vr )
		&& CreateXrSession( g_Vr )
		&& CreateSwapchains( g_Vr )
		&& CreateDepthBuffer( g_Vr )
		&& CreateRenderPass( g_Vr )
		&& CreateFramebuffers( g_Vr )
		&& CreateDescriptorStuff( g_Vr )
		&& CreatePipeline( g_Vr );

	if ( !g_Vr.ready )
		LOGE( "VR session setup failed" );
	else
		LOGI( "VR session ready: %zu eye(s), %ux%u", g_Vr.eyes.size(), g_Vr.eyes.empty() ? 0 : g_Vr.eyes[0].width, g_Vr.eyes.empty() ? 0 : g_Vr.eyes[0].height );

	return g_Vr.ready;
}

bool VRXR_BeginSceneIfNeeded()
{
	if ( !g_Vr.ready )
		return false;

	PollXrEvents( g_Vr );

	if ( g_Vr.frameOpen )
		return true;

	if ( !g_Vr.sessionRunning )
	{
		// See VRXR_EndSceneAndPresent()'s comment - busy-polling here starves
		// the compositor of the scheduling time it needs to advance the
		// session state past IDLE.
		usleep( 10000 );
		return false;
	}

	XrFrameWaitInfo waitInfo = { XR_TYPE_FRAME_WAIT_INFO };
	g_Vr.frameState = { XR_TYPE_FRAME_STATE };
	if ( !XrCheck( xrWaitFrame( g_Vr.session, &waitInfo, &g_Vr.frameState ), "xrWaitFrame" ) )
		return false;

	XrFrameBeginInfo beginInfo = { XR_TYPE_FRAME_BEGIN_INFO };
	xrBeginFrame( g_Vr.session, &beginInfo );

	uint32_t viewCount = (uint32_t)g_Vr.eyes.size();
	g_Vr.currentViews.assign( viewCount, { XR_TYPE_VIEW } );
	XrViewLocateInfo locateInfo = { XR_TYPE_VIEW_LOCATE_INFO };
	locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	locateInfo.displayTime = g_Vr.frameState.predictedDisplayTime;
	locateInfo.space = g_Vr.localSpace;
	XrViewState viewState = { XR_TYPE_VIEW_STATE };
	uint32_t viewCountOut = 0;
	xrLocateViews( g_Vr.session, &locateInfo, &viewState, viewCount, &viewCountOut, g_Vr.currentViews.data() );

	for ( uint32_t i = 0; i < viewCount; i++ )
	{
		XrSwapchainImageAcquireInfo acquireInfo = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
		xrAcquireSwapchainImage( g_Vr.eyes[i].swapchain, &acquireInfo, &g_Vr.eyeImageIndex[i] );

		XrSwapchainImageWaitInfo waitImgInfo = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
		waitImgInfo.timeout = XR_INFINITE_DURATION;
		xrWaitSwapchainImage( g_Vr.eyes[i].swapchain, &waitImgInfo );
	}

	// Record into eye 0's render pass; eye 1 gets a mono copy of the result
	// in VRXR_EndSceneAndPresent() (no per-eye stereo separation yet - the
	// engine isn't rendering per-eye views, it has one flat camera).
	g_Vr.sceneCmdBuffer = g_Vr.eyes[0].commandBuffers[g_Vr.eyeImageIndex[0]];

	VkCommandBufferBeginInfo cmdBeginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	vkBeginCommandBuffer( g_Vr.sceneCmdBuffer, &cmdBeginInfo );

	VkClearValue clearValues[2] = {};
	clearValues[0].color = { { 0.05f, 0.05f, 0.08f, 1.0f } };
	clearValues[1].depthStencil = { 1.0f, 0 };

	VkRenderPassBeginInfo rpBeginInfo = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
	rpBeginInfo.renderPass = g_Vr.renderPass;
	rpBeginInfo.framebuffer = g_Vr.eyes[0].framebuffers[g_Vr.eyeImageIndex[0]];
	rpBeginInfo.renderArea.extent = { g_Vr.eyes[0].width, g_Vr.eyes[0].height };
	rpBeginInfo.clearValueCount = 2;
	rpBeginInfo.pClearValues = clearValues;
	vkCmdBeginRenderPass( g_Vr.sceneCmdBuffer, &rpBeginInfo, VK_SUBPASS_CONTENTS_INLINE );

	VkViewport viewport = { 0, 0, (float)g_Vr.eyes[0].width, (float)g_Vr.eyes[0].height, 0.0f, 1.0f };
	VkRect2D scissor = { { 0, 0 }, { g_Vr.eyes[0].width, g_Vr.eyes[0].height } };
	vkCmdSetViewport( g_Vr.sceneCmdBuffer, 0, 1, &viewport );
	vkCmdSetScissor( g_Vr.sceneCmdBuffer, 0, 1, &scissor );
	vkCmdBindPipeline( g_Vr.sceneCmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, g_Vr.pipeline );

	g_Vr.frameOpen = true;
	return true;
}

bool VRXR_IsFrameActive()
{
	return g_Vr.frameOpen;
}

void VRXR_DrawIndexed( VkBuffer vb, VkDeviceSize vbOffset, VkBuffer ib, VkDeviceSize ibOffset,
	uint32_t indexCount, const float mvp[16], VkImageView textureView, VkSampler sampler )
{
	if ( !g_Vr.frameOpen || indexCount == 0 )
		return;

	VkDescriptorSet descSet = GetOrCreateDescriptorSet( g_Vr, textureView, sampler );
	if ( descSet == VK_NULL_HANDLE )
		return;

	VkCommandBuffer cmd = g_Vr.sceneCmdBuffer;
	vkCmdBindDescriptorSets( cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_Vr.pipelineLayout, 0, 1, &descSet, 0, NULL );
	vkCmdPushConstants( cmd, g_Vr.pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, 16 * sizeof( float ), mvp );
	vkCmdBindVertexBuffers( cmd, 0, 1, &vb, &vbOffset );
	vkCmdBindIndexBuffer( cmd, ib, ibOffset, VK_INDEX_TYPE_UINT16 );
	vkCmdDrawIndexed( cmd, indexCount, 1, 0, 0, 0 );
}

void VRXR_EndSceneAndPresent()
{
	if ( !g_Vr.ready )
		return;

	// NOTE: anything in deferredDestroy right now was deferred specifically
	// because it might still be referenced by *this* still-open frame's
	// not-yet-submitted command buffer (see CEmptyMesh::UploadToGpu() in
	// shaderapivulkan.cpp, which only defers - instead of destroying
	// immediately - while a frame is active). Flushing it here, before that
	// command buffer is submitted below, would destroy buffers it still
	// references. The flush has to wait until after this frame's
	// submission+wait completes (see bottom of this function).
	if ( !g_Vr.frameOpen )
	{
		// VRXR_BeginSceneIfNeeded() never got called, or the session wasn't
		// running yet when it was tried - nothing to present this cycle.
		return;
	}

	vkCmdEndRenderPass( g_Vr.sceneCmdBuffer );
	vkEndCommandBuffer( g_Vr.sceneCmdBuffer );

	VkSubmitInfo submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &g_Vr.sceneCmdBuffer;
	vkQueueSubmit( g_Vr.vkQueue, 1, &submitInfo, VK_NULL_HANDLE );
	vkQueueWaitIdle( g_Vr.vkQueue );

	// Mono-copy eye 0's finished frame into eye 1 - no per-eye stereo
	// separation yet (the engine renders one flat camera, not two).
	VkImage eye0Image = g_Vr.eyes[0].images[g_Vr.eyeImageIndex[0]];
	VkImage eye1Image = g_Vr.eyes[1].images[g_Vr.eyeImageIndex[1]];
	VkCommandBuffer copyCmd = g_Vr.eyes[1].commandBuffers[g_Vr.eyeImageIndex[1]];

	VkCommandBufferBeginInfo copyBeginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	vkBeginCommandBuffer( copyCmd, &copyBeginInfo );

	VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

	VkImageMemoryBarrier toSrcBarrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
	toSrcBarrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	toSrcBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	toSrcBarrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	toSrcBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	toSrcBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	toSrcBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	toSrcBarrier.image = eye0Image;
	toSrcBarrier.subresourceRange = range;

	VkImageMemoryBarrier toDstBarrier = toSrcBarrier;
	toDstBarrier.srcAccessMask = 0;
	toDstBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	toDstBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	toDstBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	toDstBarrier.image = eye1Image;

	VkImageMemoryBarrier preCopyBarriers[2] = { toSrcBarrier, toDstBarrier };
	vkCmdPipelineBarrier( copyCmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
		0, 0, NULL, 0, NULL, 2, preCopyBarriers );

	VkImageCopy copyRegion = {};
	copyRegion.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
	copyRegion.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
	copyRegion.extent = { g_Vr.eyes[1].width, g_Vr.eyes[1].height, 1 };
	vkCmdCopyImage( copyCmd, eye0Image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, eye1Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion );

	VkImageMemoryBarrier eye0Back = toSrcBarrier;
	eye0Back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	eye0Back.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	eye0Back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	eye0Back.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	VkImageMemoryBarrier eye1Ready = toDstBarrier;
	eye1Ready.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	eye1Ready.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	eye1Ready.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	eye1Ready.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	VkImageMemoryBarrier postCopyBarriers[2] = { eye0Back, eye1Ready };
	vkCmdPipelineBarrier( copyCmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		0, 0, NULL, 0, NULL, 2, postCopyBarriers );

	vkEndCommandBuffer( copyCmd );

	VkSubmitInfo copySubmit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
	copySubmit.commandBufferCount = 1;
	copySubmit.pCommandBuffers = &copyCmd;
	vkQueueSubmit( g_Vr.vkQueue, 1, &copySubmit, VK_NULL_HANDLE );
	vkQueueWaitIdle( g_Vr.vkQueue );

	std::vector<XrCompositionLayerProjectionView> projViews( g_Vr.eyes.size() );
	for ( size_t i = 0; i < g_Vr.eyes.size(); i++ )
	{
		XrSwapchainImageReleaseInfo releaseInfo = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
		xrReleaseSwapchainImage( g_Vr.eyes[i].swapchain, &releaseInfo );

		projViews[i].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
		projViews[i].pose = g_Vr.currentViews[i].pose;
		projViews[i].fov = g_Vr.currentViews[i].fov;
		projViews[i].subImage.swapchain = g_Vr.eyes[i].swapchain;
		projViews[i].subImage.imageRect.offset = { 0, 0 };
		projViews[i].subImage.imageRect.extent = { (int32_t)g_Vr.eyes[i].width, (int32_t)g_Vr.eyes[i].height };
	}

	XrCompositionLayerProjection projLayer = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
	projLayer.space = g_Vr.localSpace;
	projLayer.viewCount = (uint32_t)projViews.size();
	projLayer.views = projViews.data();
	const XrCompositionLayerBaseHeader *layers[1] = { (const XrCompositionLayerBaseHeader *)&projLayer };

	XrFrameEndInfo endInfo = { XR_TYPE_FRAME_END_INFO };
	endInfo.displayTime = g_Vr.frameState.predictedDisplayTime;
	endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	endInfo.layerCount = 1;
	endInfo.layers = layers;
	xrEndFrame( g_Vr.session, &endInfo );

	g_Vr.frameOpen = false;

	// Safe now - the vkQueueWaitIdle calls above (this frame's draw
	// submission and the eye0->eye1 copy) guarantee the GPU is done with
	// anything this now-closed frame's command buffers referenced.
	for ( auto &entry : g_Vr.deferredDestroy )
	{
		vkDestroyBuffer( g_Vr.vkDevice, entry.first, NULL );
		vkFreeMemory( g_Vr.vkDevice, entry.second, NULL );
	}
	g_Vr.deferredDestroy.clear();
}

bool VRXR_WantsExit()
{
	return g_Vr.exitRequested;
}

void VRXR_Shutdown()
{
	if ( g_Vr.session != XR_NULL_HANDLE )
	{
		if ( g_Vr.sessionRunning )
			xrEndSession( g_Vr.session );
		xrDestroySession( g_Vr.session );
		g_Vr.session = XR_NULL_HANDLE;
	}
	if ( g_Vr.vkDevice != VK_NULL_HANDLE )
	{
		vkDestroyDevice( g_Vr.vkDevice, NULL );
		g_Vr.vkDevice = VK_NULL_HANDLE;
	}
	if ( g_Vr.vkInstance != VK_NULL_HANDLE )
	{
		vkDestroyInstance( g_Vr.vkInstance, NULL );
		g_Vr.vkInstance = VK_NULL_HANDLE;
	}
	if ( g_Vr.instance != XR_NULL_HANDLE )
	{
		xrDestroyInstance( g_Vr.instance );
		g_Vr.instance = XR_NULL_HANDLE;
	}
	g_Vr.ready = false;
}

VkDevice VRXR_GetDevice() { return g_Vr.vkDevice; }
VkPhysicalDevice VRXR_GetPhysicalDevice() { return g_Vr.vkPhysicalDevice; }
VkQueue VRXR_GetQueue() { return g_Vr.vkQueue; }
uint32_t VRXR_GetQueueFamilyIndex() { return g_Vr.queueFamilyIndex; }
VkCommandPool VRXR_GetCommandPool() { return g_Vr.vkCommandPool; }
VkSampler VRXR_GetDefaultSampler() { return g_Vr.defaultSampler; }

bool VRXR_CreateBuffer( VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties,
	VkBuffer *pBuffer, VkDeviceMemory *pMemory )
{
	// Callers (mesh/texture upload code) can run before VRXR_Init() has ever
	// been called - materialsystem does real work (locking/uploading meshes
	// for UI, fonts, etc.) well before the engine's first frame Present(),
	// which is where VRXR_Init() actually happens (lazily, see
	// shaderapivulkan.cpp). Calling any vk*() function against a still-null
	// VkDevice crashes (the loader dereferences the device's dispatch
	// table), so bail out quietly instead.
	if ( g_Vr.vkDevice == VK_NULL_HANDLE )
		return false;

	VkBufferCreateInfo bufInfo = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	bufInfo.size = size;
	bufInfo.usage = usage;
	bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if ( !VkCheck( vkCreateBuffer( g_Vr.vkDevice, &bufInfo, NULL, pBuffer ), "vkCreateBuffer" ) )
		return false;

	VkMemoryRequirements memReq;
	vkGetBufferMemoryRequirements( g_Vr.vkDevice, *pBuffer, &memReq );

	VkMemoryAllocateInfo allocInfo = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	allocInfo.allocationSize = memReq.size;
	allocInfo.memoryTypeIndex = FindMemoryType( memReq.memoryTypeBits, properties );
	if ( allocInfo.memoryTypeIndex == UINT32_MAX )
	{
		LOGE( "VRXR_CreateBuffer: no matching memory type" );
		vkDestroyBuffer( g_Vr.vkDevice, *pBuffer, NULL );
		*pBuffer = VK_NULL_HANDLE;
		return false;
	}
	if ( !VkCheck( vkAllocateMemory( g_Vr.vkDevice, &allocInfo, NULL, pMemory ), "vkAllocateMemory (buffer)" ) )
	{
		vkDestroyBuffer( g_Vr.vkDevice, *pBuffer, NULL );
		*pBuffer = VK_NULL_HANDLE;
		return false;
	}
	vkBindBufferMemory( g_Vr.vkDevice, *pBuffer, *pMemory, 0 );
	return true;
}

bool VRXR_CreateImage2D( uint32_t width, uint32_t height, VkFormat format, VkImageTiling tiling,
	VkImageUsageFlags usage, VkMemoryPropertyFlags properties, VkImage *pImage, VkDeviceMemory *pMemory )
{
	// See VRXR_CreateBuffer()'s comment - same pre-VRXR_Init() hazard.
	if ( g_Vr.vkDevice == VK_NULL_HANDLE )
		return false;

	VkImageCreateInfo imageInfo = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	imageInfo.imageType = VK_IMAGE_TYPE_2D;
	imageInfo.format = format;
	imageInfo.extent = { width, height, 1 };
	imageInfo.mipLevels = 1;
	imageInfo.arrayLayers = 1;
	imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	imageInfo.tiling = tiling;
	imageInfo.usage = usage;
	imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if ( !VkCheck( vkCreateImage( g_Vr.vkDevice, &imageInfo, NULL, pImage ), "vkCreateImage" ) )
		return false;

	VkMemoryRequirements memReq;
	vkGetImageMemoryRequirements( g_Vr.vkDevice, *pImage, &memReq );

	VkMemoryAllocateInfo allocInfo = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	allocInfo.allocationSize = memReq.size;
	allocInfo.memoryTypeIndex = FindMemoryType( memReq.memoryTypeBits, properties );
	if ( allocInfo.memoryTypeIndex == UINT32_MAX )
	{
		LOGE( "VRXR_CreateImage2D: no matching memory type" );
		vkDestroyImage( g_Vr.vkDevice, *pImage, NULL );
		*pImage = VK_NULL_HANDLE;
		return false;
	}
	if ( !VkCheck( vkAllocateMemory( g_Vr.vkDevice, &allocInfo, NULL, pMemory ), "vkAllocateMemory (image)" ) )
	{
		vkDestroyImage( g_Vr.vkDevice, *pImage, NULL );
		*pImage = VK_NULL_HANDLE;
		return false;
	}
	vkBindImageMemory( g_Vr.vkDevice, *pImage, *pMemory, 0 );
	return true;
}

void VRXR_DeferredDestroyBuffer( VkBuffer buffer, VkDeviceMemory memory )
{
	g_Vr.deferredDestroy.emplace_back( buffer, memory );
}
