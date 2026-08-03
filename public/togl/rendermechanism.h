//========= Copyright Valve Corporation, All rights reserved. ============//
//                       TOGL CODE LICENSE
//
//  Copyright 2011-2014 Valve Corporation
//  All Rights Reserved.
//
//  Permission is hereby granted, free of charge, to any person obtaining a copy
//  of this software and associated documentation files (the "Software"), to deal
//  in the Software without restriction, including without limitation the rights
//  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
//  copies of the Software, and to permit persons to whom the Software is
//  furnished to do so, subject to the following conditions:
//
//  The above copyright notice and this permission notice shall be included in
//  all copies or substantial portions of the Software.
//
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
//  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
//  THE SOFTWARE.
//

#ifdef TOGLES
#include "togles/rendermechanism.h"
#else

#ifndef RENDERMECHANISM_H
#define RENDERMECHANISM_H

#if defined(DX_TO_GL_ABSTRACTION)

#undef PROTECTED_THINGS_ENABLE

#include "SDL_opengl.h"
#include "tier0/basetypes.h"
#include "tier0/platform.h"

#include "togl/linuxwin/glmdebug.h"
#include "togl/linuxwin/glbase.h"
#include "togl/linuxwin/glentrypoints.h"
#include "togl/linuxwin/glmdisplay.h"
#include "togl/linuxwin/glmdisplaydb.h"
#include "togl/linuxwin/glmgrbasics.h"
#include "togl/linuxwin/glmgrext.h"
#include "togl/linuxwin/cglmbuffer.h"
#include "togl/linuxwin/cglmtex.h"
#include "togl/linuxwin/cglmfbo.h"
#include "togl/linuxwin/cglmprogram.h"
#include "togl/linuxwin/cglmquery.h"
#include "togl/linuxwin/glmgr.h"
#include "togl/linuxwin/dxabstract_types.h"
#include "togl/linuxwin/dxabstract.h"

#else
	//USE_ACTUAL_DX
	#ifdef WIN32
		#ifdef _X360
			#include "d3d9.h"
			#include "d3dx9.h"
		#else
			#include <windows.h>
			#include "../../dx9sdk/include/d3d9.h"
			#include "../../dx9sdk/include/d3dx9.h"
		#endif
		typedef HWND VD3DHWND;
	#elif defined( DXVK_NATIVE )
		// Non-Windows target running the real D3D9 renderer via DXVK Native
		// (DirectX9-on-Vulkan). windows.h/d3d9.h here are DXVK's own portable
		// headers (vendored at dxvk_native/include/native/{windows,directx}),
		// not the real Windows SDK - they provide just enough of HWND/HRESULT/
		// IUnknown/etc for this code to compile and link against
		// libdxvk_d3d9.so, matching DXVK Native's own build. Deliberately NOT
		// including the full d3dx9.h "kitchen sink" header here - its legacy
		// mesh/font/animation authoring-tool sub-headers (d3dx9mesh.h etc,
		// not used by the runtime renderer) don't compile clean against
		// DXVK's minimal windows.h, and this build already doesn't link a
		// separate D3DX9 library for non-win32 targets (see this module's
		// wscript). d3dx9.h's math/shader sub-headers (D3DXMATRIX,
		// ID3DXBuffer, D3DXGetShaderVersion etc) ARE needed by the renderer,
		// but its legacy mesh/font/animation authoring-tool sub-headers
		// (d3dx9mesh.h/d3dx9shape.h/d3dx9anim.h, transitively pulled in by
		// d3dx9.h's own #includes - unused at runtime but not separable)
		// reference a handful of COM/GDI types DXVK's minimal windows.h
		// never defines, since DXVK's own build never touches those
		// sub-headers either. Shim just those in before pulling d3dx9.h.
		#include <windows.h>
		struct IStream;
		struct GLYPHMETRICSFLOAT;
		struct TEXTMETRICA;
		struct TEXTMETRICW;
		typedef double DOUBLE;
		typedef GUID *LPGUID;
		#define LF_FACESIZE 32
		#ifndef EXTERN_C
			#ifdef __cplusplus
				#define EXTERN_C extern "C"
			#else
				#define EXTERN_C extern
			#endif
		#endif
		#ifndef STDAPICALLTYPE
			#define STDAPICALLTYPE WINAPI
		#endif
		#ifndef STDAPI
			#define STDAPI EXTERN_C HRESULT STDAPICALLTYPE
		#endif
		#include <d3d9.h>
		#include <d3dx9.h>
		typedef HWND VD3DHWND;
	#endif

	#define	GLMPRINTF(args)	
	#define	GLMPRINTSTR(args)
	#define	GLMPRINTTEXT(args)
#endif // defined(DX_TO_GL_ABSTRACTION)

#endif // RENDERMECHANISM_H

#endif
