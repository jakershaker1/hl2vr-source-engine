//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Minimal OpenXR bootstrap for Android: loader init, instance creation,
// and HMD system discovery.
//
//===========================================================================//
#ifndef OPENXR_BOOTSTRAP_H
#define OPENXR_BOOTSTRAP_H

#define XR_USE_PLATFORM_ANDROID
#include <openxr/openxr.h>

struct android_app;

// Initializes the OpenXR loader against the Android runtime, creates an
// XrInstance, and queries the HMD XrSystemId. Logs the runtime/system name
// on success. On success, the caller owns *pInstance and must eventually
// xrDestroyInstance() it. Returns true if a usable OpenXR runtime was found.
bool InitOpenXR( struct android_app *app, XrInstance *pInstance, XrSystemId *pSystemId );

#endif // OPENXR_BOOTSTRAP_H
