//========= Copyright Valve Corporation, All rights reserved. ============//
//
//
//
//==================================================================================================
#ifndef WINUTILS_H
#define WINUTILS_H

#include "togl/rendermechanism.h" // for win types

#if !defined(_WIN32)

	void Sleep( unsigned int ms );
	bool IsIconic( VD3DHWND hWnd );
	BOOL ClientToScreen( VD3DHWND hWnd, LPPOINT pPoint );
	void* GetCurrentThread();
	void SetThreadAffinityMask( void *hThread, int nMask );
	void GlobalMemoryStatus( MEMORYSTATUS *pOut );
#if defined( DXVK_NATIVE )
	// Real windows.h provides this on WIN32; DXVK's portable windows.h
	// doesn't implement actual window queries (that's the WSI backend's
	// job) - implemented in winutils.cpp via SDL_GetWindowSize on the
	// window DXVK_GetOrCreateWindow() creates.
	void GetClientRect( VD3DHWND hWnd, RECT *pRect );

	// Creates (once) an SDL2 window with the Vulkan flag set, satisfying
	// DXVK Native's requirement for a real native window handle at D3D9
	// device-creation time (its SDL2 WSI backend builds the VkSurfaceKHR
	// from this). Returns the SDL_Window*, reinterpreted as VD3DHWND to
	// match this module's HWND convention everywhere else. We never
	// actually show or care about this window on screen - the real,
	// visible output goes through the separate OpenXR/Vulkan swapchain in
	// launcher/android/vr_xr_vulkan.cpp; this window only exists to give
	// DXVK's device/swapchain something to construct against.
	VD3DHWND DXVK_GetOrCreateWindow();
#endif
#endif

#endif // WINUTILS_H
