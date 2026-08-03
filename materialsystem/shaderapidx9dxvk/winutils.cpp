//========= Copyright Valve Corporation, All rights reserved. ============//
//
// winutils.cpp
//
//===========================================================================//

#include "winutils.h"

#ifndef _WIN32

#include <ctype.h>
#include <stdlib.h>
#include "tier0/basetypes.h"
#include "tier0/dbg.h"
#include "tier0/threadtools.h"
#include "appframework/ilaunchermgr.h"

// LINUX path taken from //Steam/main/src/tier0/platform_posix.cpp - Returns installed RAM in MB. 
static unsigned long GetInstalledRAM()
{
	unsigned long ulTotalRamMB = 2047;

#ifdef LINUX
	char rgchLine[256];
	FILE *fpMemInfo = fopen( "/proc/meminfo", "r" );
	if ( !fpMemInfo )
		return ulTotalRamMB;

	const char *pszSearchString = "MemTotal:";
	const uint cchSearchString = strlen( pszSearchString );
	while ( fgets( rgchLine, sizeof(rgchLine), fpMemInfo ) )
	{
		if ( !strncasecmp( pszSearchString, rgchLine, cchSearchString ) )
		{
			char *pszVal = rgchLine+cchSearchString;
			while( isspace(*pszVal) )
				++pszVal;
			ulTotalRamMB = atol( pszVal ) / 1024; // go from kB to MB
			break;
		}
	}
	fclose( fpMemInfo );
#endif

	// 128 Gb limit for now (should future proof us for a while)
	ulTotalRamMB = MIN( ulTotalRamMB, 1024 * 128 );
	return ulTotalRamMB;
}

void GlobalMemoryStatus( MEMORYSTATUS *pOut )
{
	unsigned long nInstalledRamInMB = GetInstalledRAM();

	// For safety assume at least 128MB
	nInstalledRamInMB = MAX( nInstalledRamInMB, 128 );

	uint64 ulTotalRam = static_cast<uint64>( nInstalledRamInMB ) * ( 1024 * 1024 );
	ulTotalRam = MIN( ulTotalRam, 0xFFFFFFFF );
	
	pOut->dwTotalPhys = static_cast<SIZE_T>( ulTotalRam );
}

void Sleep( unsigned int ms )
{
	DebuggerBreak();
	ThreadSleep( ms );
}

bool IsIconic( VD3DHWND hWnd )
{
	// FIXME for now just act non-minimized all the time
	//DebuggerBreak();
	return false;
}

BOOL ClientToScreen( VD3DHWND hWnd, LPPOINT pPoint )
{
	DebuggerBreak();
	return true;
}

void* GetCurrentThread()
{
	DebuggerBreak();
	return 0;
}

void SetThreadAffinityMask( void *hThread, int nMask )
{
	DebuggerBreak();
}

#if !defined( DXVK_NATIVE )
// Only declared in-class (and thus definable out-of-line like this) by the
// old togl/dxabstract GUID type this file originally targeted. DXVK's own
// portable GUID (windows_base.h) doesn't declare this member, and nothing
// in this module actually calls it.
bool GUID::operator==( const struct _GUID &other ) const
{
	DebuggerBreak();
	return memcmp( this, &other, sizeof( GUID ) ) == 0;
}
#endif

#if defined( DXVK_NATIVE )

// DXVK is configured to use a custom "Headless" WSI backend on Android
// (dxvk_native/src/wsi/wsi_platform.cpp, selected via DXVK_WSI_DRIVER in
// android_main.cpp) instead of SDL2's real Android video backend - the
// latter needs JNI bootstrapping via SDL's own Java Activity class, which
// this android_native_app_glue-based app never performs. Despite the name,
// that WSI driver creates a real VK_KHR_android_surface-backed VkSurfaceKHR
// against this app's actual ANativeWindow (VK_EXT_headless_surface isn't
// supported by this device's driver) - passed through opaquely as the
// VD3DHWND value here, decoded from the HL2VR_ANATIVE_WINDOW env var
// android_main.cpp sets when the real window becomes available. Safe to
// share since our own OpenXR/Vulkan rendering (vr_xr_vulkan.cpp) never
// touches the raw 2D window, only OpenXR's own swapchain.
VD3DHWND DXVK_GetOrCreateWindow()
{
	const char *pWindowHex = getenv( "HL2VR_ANATIVE_WINDOW" );
	if ( !pWindowHex )
		return NULL;

	return (VD3DHWND)(uintptr_t)strtoull( pWindowHex, NULL, 16 );
}

void GetClientRect( VD3DHWND hWnd, RECT *pRect )
{
	pRect->left = 0;
	pRect->top = 0;
	pRect->right = 1280;
	pRect->bottom = 720;
}
#endif
#endif
