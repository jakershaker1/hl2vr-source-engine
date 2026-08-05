//========= Copyright Valve Corporation, All rights reserved. ============//
//
// OpenXR + OpenGL ES session/swapchain plumbing. Binds an XrSession to the
// EGL display/config/context appframework/sdlmgr.cpp (CSDLMgr) already
// created for togles/shaderapidx9, creates per-eye swapchains, and installs
// itself as CSDLMgr's frame-present hook (see sdlmgr.cpp's
// g_pfnHL2VR_PresentFrame) so every completed D3D9 backbuffer gets blitted
// into the OpenXR compositor automatically - no changes to togles or
// shaderapidx9 needed.
//
//===========================================================================//
#ifndef VR_XR_GLES_H
#define VR_XR_GLES_H

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <jni.h>
#include <EGL/egl.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

// Queries GLES version requirements, creates the XrSession bound to
// CSDLMgr's existing EGL display/config/context, a local reference space,
// and per-eye swapchains sized to the runtime's recommended resolution.
// Takes ownership of `instance` (destroyed by VRXR_Shutdown). Returns false
// on failure.
bool VRXR_Init( XrInstance instance, XrSystemId systemId );

// Pumps xrPollEvent and advances the session state machine
// (READY -> xrBeginSession, STOPPING -> xrEndSession, etc). Safe/cheap to
// call every engine frame even before the session is running.
void VRXR_PumpEvents();

void VRXR_Shutdown();

// True once VRXR_Init() has succeeded and the session is currently running
// (headset worn / focused) - i.e. it's meaningful to call VRXR_BeginFrame().
bool VRXR_IsRunning();

// Head/eye pose, in OpenXR space (meters; X=right, Y=up, Z=backward,
// right-handed) - the same convention OpenVR uses, so the existing
// OpenVR->Source coordinate conversion applies unchanged.
struct VRXR_Pose_t
{
	float px, py, pz;       // position
	float qx, qy, qz, qw;   // orientation quaternion
};

struct VRXR_Fov_t
{
	float angleLeft, angleRight, angleUp, angleDown; // radians
};

// Called once per engine frame - from ISourceVirtualReality::SampleTrackingState
// (see vr_sourcevr_xr.cpp), which the engine's own VR camera code already
// calls once per frame before setting up the left/right eye views. Runs
// xrWaitFrame/xrBeginFrame/xrLocateViews and caches the results for this
// frame's VRXR_GetEyePose()/VRXR_GetEyeFov() calls. Returns false (and
// leaves the previous frame's cached pose/fov in place) if the session
// isn't running yet.
bool VRXR_BeginFrame();

VRXR_Pose_t VRXR_GetEyePose( int eye ); // 0 = left, 1 = right
VRXR_Fov_t VRXR_GetEyeFov( int eye );

#endif // VR_XR_GLES_H
