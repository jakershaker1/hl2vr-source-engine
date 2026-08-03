//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Bridges DXVK's rendered D3D9 frame into the OpenXR swapchain. DXVK owns
// its own separate Vulkan instance/device/swapchain (tied to the SDL2
// window from winutils.cpp's DXVK_GetOrCreateWindow) - this reads the
// finished frame back to CPU memory via the plain D3D9 API
// (GetRenderTargetData/LockRect, no DXVK/Vulkan internals touched) and
// re-uploads it into our own, separately-managed OpenXR/Vulkan session
// (launcher/android/vr_xr_vulkan.{h,cpp}) as a full-screen textured quad.
// Simplest correct bridge, not the fastest one - see Phase 2 of
// dapper-weaving-prism.md.
//
//===========================================================================//
#ifndef DXVK_XR_BRIDGE_H
#define DXVK_XR_BRIDGE_H

struct IDirect3DDevice9;

void DXVK_PresentFrameToXR( IDirect3DDevice9 *pDevice );

#endif // DXVK_XR_BRIDGE_H
