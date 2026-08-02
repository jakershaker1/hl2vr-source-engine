//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Reusable OpenXR + Vulkan device/session/swapchain plumbing, shared
// between the engine's real renderer (materialsystem/shaderapivulkan,
// which drives this from ClearBuffers()/IMesh::Draw()/Present()) and any
// other Android entry point that needs it. This module owns exactly one
// VR session, one fixed graphics pipeline, and one in-flight frame at a
// time.
//
//===========================================================================//
#ifndef VR_XR_VULKAN_H
#define VR_XR_VULKAN_H

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_VULKAN
#include <jni.h>
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

// Creates the Vulkan instance/device satisfying OpenXR's requirements,
// binds them into a new XrSession, creates per-eye swapchains sized to the
// runtime's recommended resolution, and builds the fixed render
// pass/pipeline used for all draws. Takes ownership of `instance`
// (destroyed by VRXR_Shutdown). Returns false on failure.
bool VRXR_Init( XrInstance instance, XrSystemId systemId );

// Idempotent - safe to call every frame (e.g. from IShaderAPI::ClearBuffers).
// If the XR session is running and no frame is currently open, pumps
// session-state events, starts a new XR frame, acquires both eyes'
// swapchain images, and begins recording into eye 0's render pass. No-op
// (and returns false) if the session isn't running yet or a frame is
// already open.
bool VRXR_BeginSceneIfNeeded();

// True between a successful VRXR_BeginSceneIfNeeded() and the matching
// VRXR_EndSceneAndPresent() - i.e. whether there's an active command
// buffer to record draws into right now.
bool VRXR_IsFrameActive();

// Ends the render pass/command buffer for eye 0, submits it, waits, copies
// the result into eye 1 (mono for now - stereo separation is later work),
// submits eye 1, and calls xrEndFrame with both. Also handles the "no
// frame was open this call" case by pumping events/yielding, matching the
// old standalone present-frame behavior, so it's safe to call every engine
// frame regardless of whether VRXR_BeginSceneIfNeeded() actually opened one.
void VRXR_EndSceneAndPresent();

// True once the runtime has asked the app to exit (session loss/exiting).
bool VRXR_WantsExit();

void VRXR_Shutdown();

// --- Recording draws into the currently-open scene (see VRXR_IsFrameActive) ---

// Records one indexed draw using the fixed pipeline: binds vb/ib, pushes
// `mvp` (column-major, 16 floats) as the vertex shader's push constant,
// binds `textureView`+`sampler` at descriptor set 0/binding 0, and issues
// vkCmdDrawIndexed. No-op if no frame is active.
void VRXR_DrawIndexed( VkBuffer vb, VkDeviceSize vbOffset, VkBuffer ib, VkDeviceSize ibOffset,
	uint32_t indexCount, const float mvp[16], VkImageView textureView, VkSampler sampler );

// --- Generic Vulkan helpers (device/memory access for shaderapivulkan's
//     own texture/buffer creation) ---

VkDevice VRXR_GetDevice();
VkPhysicalDevice VRXR_GetPhysicalDevice();
VkQueue VRXR_GetQueue();
uint32_t VRXR_GetQueueFamilyIndex();
VkCommandPool VRXR_GetCommandPool();

// The sampler used for all draws recorded via VRXR_DrawIndexed() - exposed
// so callers building their own descriptor-free one-shot uploads (e.g.
// shaderapivulkan's fallback white texture) can reuse it instead of
// creating a duplicate.
VkSampler VRXR_GetDefaultSampler();

// Allocates+binds device memory for a buffer/image from a memory type
// matching `properties`. Returns false on failure.
bool VRXR_CreateBuffer( VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties,
	VkBuffer *pBuffer, VkDeviceMemory *pMemory );
bool VRXR_CreateImage2D( uint32_t width, uint32_t height, VkFormat format, VkImageTiling tiling,
	VkImageUsageFlags usage, VkMemoryPropertyFlags properties, VkImage *pImage, VkDeviceMemory *pMemory );

// Queues (buffer,memory) or (image,view,memory) for destruction once the
// current in-flight frame's GPU work is known to have completed (right
// after the next VRXR_EndSceneAndPresent()'s wait-idle). Meshes in this
// codebase are re-locked and re-drawn from scratch essentially every use,
// so per-draw buffers are created and thrown away constantly - deferring
// destruction avoids freeing memory the GPU might still be reading.
void VRXR_DeferredDestroyBuffer( VkBuffer buffer, VkDeviceMemory memory );

#endif // VR_XR_VULKAN_H
