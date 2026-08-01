//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Minimal OpenXR bootstrap for Android: loader init, instance creation,
// and HMD system discovery. No graphics binding / session yet - that
// needs a Vulkan device (see task #6/#7).
//
//===========================================================================//
#ifndef OPENXR_BOOTSTRAP_H
#define OPENXR_BOOTSTRAP_H

struct android_app;

// Initializes the OpenXR loader against the Android runtime, creates an
// XrInstance, and queries the HMD XrSystemId. Logs the runtime/system name
// on success. Returns true if a usable OpenXR runtime was found.
bool InitOpenXR( struct android_app *app );

#endif // OPENXR_BOOTSTRAP_H
