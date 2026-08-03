//========= Copyright Valve Corporation, All rights reserved. ============//
//
// See dxvk_format_fix.h. Same conversion logic as bitmap/imageformat.cpp's
// ImageLoader::ImageFormatToD3DFormat()/D3DFormatToImageFormat() (non-X360,
// non-TOGLES paths only - not applicable to this target), just compiled
// here so it sees the real D3DFORMAT values.
//
//===========================================================================//
#include "dxvk_format_fix.h"

namespace DxvkFormat
{

ImageFormat D3DFormatToImageFormat( D3DFORMAT format )
{
	switch ( format )
	{
	case D3DFMT_R8G8B8:
		return IMAGE_FORMAT_BGR888;
	case D3DFMT_A8R8G8B8:
		return IMAGE_FORMAT_BGRA8888;
	case D3DFMT_X8R8G8B8:
		return IMAGE_FORMAT_BGRX8888;
	case D3DFMT_R5G6B5:
		return IMAGE_FORMAT_BGR565;
	case D3DFMT_X1R5G5B5:
		return IMAGE_FORMAT_BGRX5551;
	case D3DFMT_A1R5G5B5:
		return IMAGE_FORMAT_BGRA5551;
	case D3DFMT_A4R4G4B4:
		return IMAGE_FORMAT_BGRA4444;
	case D3DFMT_L8:
		return IMAGE_FORMAT_I8;
	case D3DFMT_A8L8:
		return IMAGE_FORMAT_IA88;
	case D3DFMT_A8:
		return IMAGE_FORMAT_A8;
	case D3DFMT_DXT1:
		return IMAGE_FORMAT_DXT1;
	case D3DFMT_DXT3:
		return IMAGE_FORMAT_DXT3;
	case D3DFMT_DXT5:
		return IMAGE_FORMAT_DXT5;
	case D3DFMT_V8U8:
		return IMAGE_FORMAT_UV88;
	case D3DFMT_Q8W8V8U8:
		return IMAGE_FORMAT_UVWQ8888;
	case D3DFMT_X8L8V8U8:
		return IMAGE_FORMAT_UVLX8888;
	case D3DFMT_A16B16G16R16F:
		return IMAGE_FORMAT_RGBA16161616F;
	case D3DFMT_A16B16G16R16:
		return IMAGE_FORMAT_RGBA16161616;
	case D3DFMT_R32F:
		return IMAGE_FORMAT_R32F;
	case D3DFMT_A32B32G32R32F:
		return IMAGE_FORMAT_RGBA32323232F;

	// DST and FOURCC formats mapped back to ImageFormat (for vendor-dependent shadow depth textures)
	case (D3DFORMAT)(MAKEFOURCC('R','A','W','Z')):
		return IMAGE_FORMAT_NV_RAWZ;
	case (D3DFORMAT)(MAKEFOURCC('I','N','T','Z')):
		return IMAGE_FORMAT_NV_INTZ;
	case (D3DFORMAT)(MAKEFOURCC('N','U','L','L')):
		return IMAGE_FORMAT_NV_NULL;
	case D3DFMT_D16:
		return IMAGE_FORMAT_NV_DST16;
	case D3DFMT_D24S8:
		return IMAGE_FORMAT_NV_DST24;
	case (D3DFORMAT)(MAKEFOURCC('D','F','1','6')):
		return IMAGE_FORMAT_ATI_DST16;
	case (D3DFORMAT)(MAKEFOURCC('D','F','2','4')):
		return IMAGE_FORMAT_ATI_DST24;

	// ATIxN FOURCC formats mapped back to ImageFormat
	case (D3DFORMAT)(MAKEFOURCC('A','T','I','1')):
		return IMAGE_FORMAT_ATI1N;
	case (D3DFORMAT)(MAKEFOURCC('A','T','I','2')):
		return IMAGE_FORMAT_ATI2N;
	}

	return IMAGE_FORMAT_UNKNOWN;
}

D3DFORMAT ImageFormatToD3DFormat( ImageFormat format )
{
	switch ( format )
	{
	case IMAGE_FORMAT_BGR888:
		return D3DFMT_R8G8B8;
	case IMAGE_FORMAT_BGRA8888:
		return D3DFMT_A8R8G8B8;
	case IMAGE_FORMAT_RGB888:
		return D3DFMT_R8G8B8;
	case IMAGE_FORMAT_RGBA8888:
		return D3DFMT_A8R8G8B8;
	case IMAGE_FORMAT_BGRX8888:
		return D3DFMT_X8R8G8B8;
	case IMAGE_FORMAT_BGR565:
		return D3DFMT_R5G6B5;
	case IMAGE_FORMAT_BGRX5551:
		return D3DFMT_X1R5G5B5;
	case IMAGE_FORMAT_BGRA5551:
		return D3DFMT_A1R5G5B5;
	case IMAGE_FORMAT_BGRA4444:
		return D3DFMT_A4R4G4B4;
	case IMAGE_FORMAT_I8:
		return D3DFMT_L8;
	case IMAGE_FORMAT_IA88:
		return D3DFMT_A8L8;
	case IMAGE_FORMAT_A8:
		return D3DFMT_A8;
	case IMAGE_FORMAT_DXT1:
	case IMAGE_FORMAT_DXT1_ONEBITALPHA:
		return D3DFMT_DXT1;
	case IMAGE_FORMAT_DXT3:
		return D3DFMT_DXT3;
	case IMAGE_FORMAT_DXT5:
		return D3DFMT_DXT5;
	case IMAGE_FORMAT_UV88:
		return D3DFMT_V8U8;
	case IMAGE_FORMAT_UVWQ8888:
		return D3DFMT_Q8W8V8U8;
	case IMAGE_FORMAT_UVLX8888:
		return D3DFMT_X8L8V8U8;
	case IMAGE_FORMAT_RGBA16161616F:
		return D3DFMT_A16B16G16R16F;
	case IMAGE_FORMAT_RGBA16161616:
		return D3DFMT_A16B16G16R16;
	case IMAGE_FORMAT_R32F:
		return D3DFMT_R32F;
	case IMAGE_FORMAT_RGBA32323232F:
		return D3DFMT_A32B32G32R32F;

	// ImageFormat mapped to vendor-dependent FOURCC formats (for shadow depth textures)
	case IMAGE_FORMAT_NV_RAWZ:
		return (D3DFORMAT)(MAKEFOURCC('R','A','W','Z'));
	case IMAGE_FORMAT_NV_INTZ:
		return (D3DFORMAT)(MAKEFOURCC('I','N','T','Z'));
	case IMAGE_FORMAT_NV_NULL:
		return (D3DFORMAT)(MAKEFOURCC('N','U','L','L'));
	case IMAGE_FORMAT_NV_DST16:
		return D3DFMT_D16;
	case IMAGE_FORMAT_NV_DST24:
		return D3DFMT_D24S8;
	case IMAGE_FORMAT_ATI_DST16:
		return (D3DFORMAT)(MAKEFOURCC('D','F','1','6'));
	case IMAGE_FORMAT_ATI_DST24:
		return (D3DFORMAT)(MAKEFOURCC('D','F','2','4'));

	// ImageFormats mapped to ATIxN FOURCC
	case IMAGE_FORMAT_ATI1N:
		return (D3DFORMAT)(MAKEFOURCC('A','T','I','1'));
	case IMAGE_FORMAT_ATI2N:
		return (D3DFORMAT)(MAKEFOURCC('A','T','I','2'));

	case IMAGE_FORMAT_DXT1_RUNTIME:
		return D3DFMT_DXT1;
	case IMAGE_FORMAT_DXT5_RUNTIME:
		return D3DFMT_DXT5;
	}

	return D3DFMT_UNKNOWN;
}

} // namespace DxvkFormat
