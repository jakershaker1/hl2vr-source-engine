//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Reusable OpenXR + Vulkan device/session/swapchain plumbing, shared
// between the engine's real renderer (materialsystem/shaderapivulkan,
// which drives this from Present()) and any other Android entry point
// that needs it. This module owns exactly one VR session at a time.
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
// binds them into a new XrSession, and creates per-eye swapchains sized to
// the runtime's recommended resolution. Takes ownership of `instance`
// (destroyed by VRXR_Shutdown). Returns false (and leaves nothing to tear
// down) on failure.
bool VRXR_Init( XrInstance instance, XrSystemId systemId );

// Call once per engine frame (e.g. from IShaderDevice::Present()). Pumps
// OpenXR session-state events and, once the session is running, submits one
// stereo frame - currently an animated clear color per eye, since there's
// no real draw submission path wired up yet.
void VRXR_PresentFrame();

// True once the runtime has asked the app to exit (session loss/exiting).
bool VRXR_WantsExit();

void VRXR_Shutdown();

#endif // VR_XR_VULKAN_H
