//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Minimal OpenXR + Vulkan presentation loop: proves the whole VR pipeline
// (instance -> Vulkan device -> session -> swapchains -> frame loop) works
// end to end by clearing each eye to an animated color. Not wired into the
// engine's materialsystem/shaderapi yet - that's the much bigger follow-up
// (routing actual game rendering through Vulkan into these swapchains).
//
//===========================================================================//
#ifndef VR_SESSION_H
#define VR_SESSION_H

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_VULKAN
#include <jni.h>
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

struct android_app;

// Runs the full session lifecycle (create Vulkan device, create session,
// swapchains, frame loop) until the app is destroyed. Blocking - run this on
// its own thread. Takes ownership of - and destroys - the XrInstance.
void RunVRSession( struct android_app *app, XrInstance instance, XrSystemId systemId );

#endif // VR_SESSION_H
