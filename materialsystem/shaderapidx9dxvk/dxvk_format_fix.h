//========= Copyright Valve Corporation, All rights reserved. ============//
//
// materialsystem/bitmap is a separate, pre-compiled shared static library
// used by many Android modules that don't know about DXVK_NATIVE - its own
// compilation of imageformat.cpp never sees DXVK's real D3DFORMAT enum
// values, only the older POSIX/DX_TO_GL_ABSTRACTION placeholder enum
// (public/bitmap/imageformat.h), where e.g. D3DFMT_A8R8G8B8 = 3 instead of
// the real D3D9 value 21. Since that library is already-compiled machine
// code, our own DXVK_NATIVE define can't change what it returns - its
// ImageLoader::ImageFormatToD3DFormat()/D3DFormatToImageFormat() silently
// return values baked in from the wrong enum. Confirmed on-device: this
// broke backbuffer format negotiation (DXVK rejected an "unsupported"
// format that was really just the wrong number for a format it actually
// supports fine).
//
// This reimplements the same conversion tables locally, compiled as part
// of this module (so it sees the real, DXVK-correct D3DFORMAT values) -
// callers in this module should use these instead of ImageLoader's.
//
//===========================================================================//
#ifndef DXVK_FORMAT_FIX_H
#define DXVK_FORMAT_FIX_H

#include "bitmap/imageformat.h"
#include "locald3dtypes.h"

namespace DxvkFormat
{
	D3DFORMAT ImageFormatToD3DFormat( ImageFormat format );
	ImageFormat D3DFormatToImageFormat( D3DFORMAT format );
}

#endif // DXVK_FORMAT_FIX_H
