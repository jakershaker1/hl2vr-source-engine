//========= Copyright Valve Corporation, All rights reserved. ============//
//
// See dxvk_xr_bridge.h.
//
//===========================================================================//
#include "dxvk_xr_bridge.h"
#include "locald3dtypes.h"
#include "../../launcher/android/vr_xr_vulkan.h"
#include <stdlib.h>
#include <string.h>
// Deliberately not <vector> - this project's tier0/basetypes.h #defines
// NULL to plain 0 (not nullptr), which collides with libc++'s internal use
// of NULL inside <vector>'s transitive includes (<algorithm>/<memory>/
// <optional>) and breaks compilation. Use a plain reusable heap buffer
// instead, matching this codebase's general avoidance of STL containers.

static bool s_bTriedXrInit = false;
static bool s_bXrReady = false;

static IDirect3DSurface9 *s_pSysMemSurface = NULL;
static UINT s_nSysMemWidth = 0, s_nSysMemHeight = 0;

static VkImage s_FrameImage = VK_NULL_HANDLE;
static VkDeviceMemory s_FrameImageMemory = VK_NULL_HANDLE;
static VkImageView s_FrameImageView = VK_NULL_HANDLE;
static uint32_t s_nFrameImageWidth = 0, s_nFrameImageHeight = 0;

static VkBuffer s_QuadVB = VK_NULL_HANDLE, s_QuadIB = VK_NULL_HANDLE;
static VkDeviceMemory s_QuadVBMem = VK_NULL_HANDLE, s_QuadIBMem = VK_NULL_HANDLE;

//-----------------------------------------------------------------------------
// Same staging-buffer upload pattern as shaderapivulkan.cpp's
// UploadRgba8ToImage (kept as reference there) - transfer-dst, copy,
// shader-read-only. Frame data here is BGRA8 (D3DFMT_A8R8G8B8's byte
// order), matching s_FrameImage's VK_FORMAT_B8G8R8A8_UNORM, so no
// per-pixel channel swizzling is needed.
//-----------------------------------------------------------------------------
static bool UploadFrameToImage( VkImage image, uint32_t width, uint32_t height, const void *pBgra8Data )
{
	VkDeviceSize size = (VkDeviceSize)width * height * 4;

	VkBuffer stagingBuffer = VK_NULL_HANDLE;
	VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
	if ( !VRXR_CreateBuffer( size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &stagingBuffer, &stagingMemory ) )
		return false;

	void *pData = NULL;
	vkMapMemory( VRXR_GetDevice(), stagingMemory, 0, size, 0, &pData );
	memcpy( pData, pBgra8Data, (size_t)size );
	vkUnmapMemory( VRXR_GetDevice(), stagingMemory );

	VkCommandBufferAllocateInfo allocInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
	allocInfo.commandPool = VRXR_GetCommandPool();
	allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocInfo.commandBufferCount = 1;
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	vkAllocateCommandBuffers( VRXR_GetDevice(), &allocInfo, &cmd );

	VkCommandBufferBeginInfo beginInfo = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer( cmd, &beginInfo );

	VkImageMemoryBarrier toTransferDst = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
	toTransferDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	toTransferDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	toTransferDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	toTransferDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	toTransferDst.image = image;
	toTransferDst.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	toTransferDst.subresourceRange.levelCount = 1;
	toTransferDst.subresourceRange.layerCount = 1;
	toTransferDst.srcAccessMask = 0;
	toTransferDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	vkCmdPipelineBarrier( cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
		0, 0, NULL, 0, NULL, 1, &toTransferDst );

	VkBufferImageCopy copyRegion = {};
	copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	copyRegion.imageSubresource.layerCount = 1;
	copyRegion.imageExtent.width = width;
	copyRegion.imageExtent.height = height;
	copyRegion.imageExtent.depth = 1;
	vkCmdCopyBufferToImage( cmd, stagingBuffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion );

	VkImageMemoryBarrier toShaderRead = toTransferDst;
	toShaderRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	toShaderRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	toShaderRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	toShaderRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	vkCmdPipelineBarrier( cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		0, 0, NULL, 0, NULL, 1, &toShaderRead );

	vkEndCommandBuffer( cmd );

	VkSubmitInfo submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &cmd;
	vkQueueSubmit( VRXR_GetQueue(), 1, &submitInfo, VK_NULL_HANDLE );
	vkQueueWaitIdle( VRXR_GetQueue() );

	vkFreeCommandBuffers( VRXR_GetDevice(), VRXR_GetCommandPool(), 1, &cmd );
	vkDestroyBuffer( VRXR_GetDevice(), stagingBuffer, NULL );
	vkFreeMemory( VRXR_GetDevice(), stagingMemory, NULL );

	return true;
}

static void EnsureQuadBuffers()
{
	if ( s_QuadVB != VK_NULL_HANDLE )
		return;

	struct QuadVertex { float x, y, z; float u, v; };
	// NDC-space full-screen quad; identity MVP below leaves it untouched.
	static const QuadVertex verts[4] = {
		{ -1, -1, 0,  0, 0 },
		{  1, -1, 0,  1, 0 },
		{  1,  1, 0,  1, 1 },
		{ -1,  1, 0,  0, 1 },
	};
	static const uint16_t indices[6] = { 0, 1, 2, 0, 2, 3 };

	VkDeviceSize vbSize = sizeof( verts );
	VkDeviceSize ibSize = sizeof( indices );

	if ( !VRXR_CreateBuffer( vbSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &s_QuadVB, &s_QuadVBMem ) ||
		 !VRXR_CreateBuffer( ibSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &s_QuadIB, &s_QuadIBMem ) )
		return;

	void *p = NULL;
	vkMapMemory( VRXR_GetDevice(), s_QuadVBMem, 0, vbSize, 0, &p );
	memcpy( p, verts, (size_t)vbSize );
	vkUnmapMemory( VRXR_GetDevice(), s_QuadVBMem );

	vkMapMemory( VRXR_GetDevice(), s_QuadIBMem, 0, ibSize, 0, &p );
	memcpy( p, indices, (size_t)ibSize );
	vkUnmapMemory( VRXR_GetDevice(), s_QuadIBMem );
}

void DXVK_PresentFrameToXR( IDirect3DDevice9 *pDevice )
{
	if ( !pDevice )
		return;

	if ( !s_bTriedXrInit )
	{
		s_bTriedXrInit = true;

		const char *pInstanceHex = getenv( "HL2VR_XR_INSTANCE" );
		const char *pSystemIdHex = getenv( "HL2VR_XR_SYSTEM_ID" );
		if ( pInstanceHex && pSystemIdHex )
		{
			XrInstance instance = (XrInstance)(uintptr_t)strtoull( pInstanceHex, NULL, 16 );
			XrSystemId systemId = (XrSystemId)strtoull( pSystemIdHex, NULL, 16 );
			s_bXrReady = VRXR_Init( instance, systemId );
		}
	}

	if ( !s_bXrReady )
		return;

	IDirect3DSurface9 *pBackBuffer = NULL;
	if ( FAILED( pDevice->GetRenderTarget( 0, &pBackBuffer ) ) || !pBackBuffer )
		return;

	D3DSURFACE_DESC desc;
	pBackBuffer->GetDesc( &desc );

	if ( !s_pSysMemSurface || s_nSysMemWidth != desc.Width || s_nSysMemHeight != desc.Height )
	{
		if ( s_pSysMemSurface )
		{
			s_pSysMemSurface->Release();
			s_pSysMemSurface = NULL;
		}
		if ( FAILED( pDevice->CreateOffscreenPlainSurface( desc.Width, desc.Height, D3DFMT_A8R8G8B8,
				D3DPOOL_SYSTEMMEM, &s_pSysMemSurface, NULL ) ) )
		{
			pBackBuffer->Release();
			return;
		}
		s_nSysMemWidth = desc.Width;
		s_nSysMemHeight = desc.Height;
	}

	HRESULT hr = pDevice->GetRenderTargetData( pBackBuffer, s_pSysMemSurface );
	pBackBuffer->Release();
	if ( FAILED( hr ) )
		return;

	D3DLOCKED_RECT locked;
	if ( FAILED( s_pSysMemSurface->LockRect( &locked, NULL, D3DLOCK_READONLY ) ) )
		return;

	if ( s_FrameImage == VK_NULL_HANDLE || s_nFrameImageWidth != desc.Width || s_nFrameImageHeight != desc.Height )
	{
		if ( s_FrameImageView != VK_NULL_HANDLE )
		{
			vkDestroyImageView( VRXR_GetDevice(), s_FrameImageView, NULL );
			s_FrameImageView = VK_NULL_HANDLE;
		}
		if ( s_FrameImage != VK_NULL_HANDLE )
		{
			vkDestroyImage( VRXR_GetDevice(), s_FrameImage, NULL );
			vkFreeMemory( VRXR_GetDevice(), s_FrameImageMemory, NULL );
			s_FrameImage = VK_NULL_HANDLE;
		}

		if ( !VRXR_CreateImage2D( desc.Width, desc.Height, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_TILING_OPTIMAL,
				VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
				VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &s_FrameImage, &s_FrameImageMemory ) )
		{
			s_pSysMemSurface->UnlockRect();
			return;
		}

		VkImageViewCreateInfo viewInfo = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
		viewInfo.image = s_FrameImage;
		viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		viewInfo.format = VK_FORMAT_B8G8R8A8_UNORM;
		viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		viewInfo.subresourceRange.levelCount = 1;
		viewInfo.subresourceRange.layerCount = 1;
		vkCreateImageView( VRXR_GetDevice(), &viewInfo, NULL, &s_FrameImageView );

		s_nFrameImageWidth = desc.Width;
		s_nFrameImageHeight = desc.Height;
	}

	// D3D9's LockRect Pitch can include row padding - repack tightly if so,
	// since the upload path assumes width*4-byte rows.
	if ( (UINT)locked.Pitch == desc.Width * 4 )
	{
		UploadFrameToImage( s_FrameImage, desc.Width, desc.Height, locked.pBits );
	}
	else
	{
		static unsigned char *s_pPackedScratch = NULL;
		static size_t s_nPackedScratchSize = 0;

		size_t nNeeded = (size_t)desc.Width * desc.Height * 4;
		if ( nNeeded > s_nPackedScratchSize )
		{
			free( s_pPackedScratch );
			s_pPackedScratch = (unsigned char *)malloc( nNeeded );
			s_nPackedScratchSize = s_pPackedScratch ? nNeeded : 0;
		}

		if ( s_pPackedScratch )
		{
			for ( UINT y = 0; y < desc.Height; ++y )
			{
				memcpy( s_pPackedScratch + (size_t)y * desc.Width * 4,
					(const unsigned char *)locked.pBits + (size_t)y * locked.Pitch,
					(size_t)desc.Width * 4 );
			}
			UploadFrameToImage( s_FrameImage, desc.Width, desc.Height, s_pPackedScratch );
		}
	}

	s_pSysMemSurface->UnlockRect();

	EnsureQuadBuffers();

	if ( VRXR_BeginSceneIfNeeded() )
	{
		static const float identity[16] = {
			1, 0, 0, 0,
			0, 1, 0, 0,
			0, 0, 1, 0,
			0, 0, 0, 1
		};
		VRXR_DrawIndexed( s_QuadVB, 0, s_QuadIB, 0, 6, identity, s_FrameImageView, VRXR_GetDefaultSampler() );
	}
	VRXR_EndSceneAndPresent();
}
