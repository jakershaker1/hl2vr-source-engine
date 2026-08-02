//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: 
//
// $NoKeywords: $
//
//===========================================================================//

#include "utlvector.h"
#include "materialsystem/imaterialsystem.h"
#include "IHardwareConfigInternal.h"
#include "shadersystem.h"
#include "shaderapi/ishaderutil.h"
#include "shaderapi/ishaderapi.h"
#include "materialsystem/imesh.h"
#include "tier0/dbg.h"
#include "materialsystem/idebugtextureinfo.h"
#include "materialsystem/deformations.h"

// tier0/basetypes.h #defines nullptr to 0 for musl-flavored Linux builds
// (`!PLATFORM_GLIBC && LINUX`) - that condition is also true for Android,
// even though NDK Clang has no need for the workaround. Left in place, it
// silently corrupts every real nullptr later in this translation unit,
// including inside libc++'s own std::vector template internals (manifests
// as bogus "cannot initialize ... with an rvalue of type 'int'" errors deep
// in vector's __compressed_pair machinery). Nothing above this point in
// this file relies on the fake macro - it only ever uses NULL/VK_NULL_HANDLE
// explicitly - so undoing it here is safe.
#ifdef nullptr
#undef nullptr
#endif

#if defined( ANDROID )
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <vector>
#include "pixelwriter.h"
#include <android/log.h>
#include "vr_xr_vulkan.h"

#define HL2VR_DIAG_TAG "hl2vr.smvk"
#define HL2VR_DIAG( ... ) __android_log_print( ANDROID_LOG_INFO, HL2VR_DIAG_TAG, __VA_ARGS__ )

//-----------------------------------------------------------------------------
// Minimal CPU-side render state shared by CEmptyMesh/CShaderAPIEmpty: a
// per-MaterialMatrixMode_t matrix stack (feeds the MVP pushed to
// basic.vert) and a table of texture handles (currently just bookkeeping -
// every draw samples a single fallback white texture; real per-material
// texture upload is future work, see VRXR_CreateImage2D).
//-----------------------------------------------------------------------------
namespace
{
	// 16 floats, row-major ( m[row*4+col] ), column-vector convention
	// ( v' = M * v ), matching public/mathlib/vmatrix.h's VMatrix layout -
	// that's the convention the engine's LoadMatrix()/MultMatrix() calls
	// assume. Transposed to GLSL's column-major layout only at the very end,
	// when building the push constant in ComputeMvp().
	typedef float Mat4[16];

	void Mat4Identity( Mat4 m )
	{
		memset( m, 0, sizeof( Mat4 ) );
		m[0] = m[5] = m[10] = m[15] = 1.0f;
	}

	void Mat4Mul( Mat4 out, const Mat4 a, const Mat4 b )
	{
		Mat4 tmp;
		for ( int r = 0; r < 4; ++r )
		{
			for ( int c = 0; c < 4; ++c )
			{
				float sum = 0.0f;
				for ( int k = 0; k < 4; ++k )
					sum += a[r * 4 + k] * b[k * 4 + c];
				tmp[r * 4 + c] = sum;
			}
		}
		memcpy( out, tmp, sizeof( Mat4 ) );
	}

	void Mat4Transpose( float dst[16], const Mat4 src )
	{
		for ( int r = 0; r < 4; ++r )
			for ( int c = 0; c < 4; ++c )
				dst[c * 4 + r] = src[r * 4 + c];
	}

	struct MatrixStackState
	{
		Mat4 stack[24];
		int depth;
	};

	MatrixStackState g_MatStacks[NUM_MATRIX_MODES];
	MaterialMatrixMode_t g_CurMatMode = MATERIAL_MODEL;

	bool g_MatStacksInit = false;
	void EnsureMatStacksInit()
	{
		if ( g_MatStacksInit )
			return;
		g_MatStacksInit = true;
		for ( int i = 0; i < NUM_MATRIX_MODES; ++i )
		{
			g_MatStacks[i].depth = 0;
			Mat4Identity( g_MatStacks[i].stack[0] );
		}
	}

	Mat4 &CurMat( MaterialMatrixMode_t mode )
	{
		EnsureMatStacksInit();
		MatrixStackState &s = g_MatStacks[mode];
		return s.stack[s.depth];
	}

	// Builds the final push-constant MVP (GLSL column-major) from the
	// independent view/projection/model stacks' current tops.
	void ComputeMvp( float dstColumnMajor[16] )
	{
		EnsureMatStacksInit();
		Mat4 vp, mvp;
		Mat4Mul( vp, g_MatStacks[MATERIAL_PROJECTION].stack[g_MatStacks[MATERIAL_PROJECTION].depth],
			g_MatStacks[MATERIAL_VIEW].stack[g_MatStacks[MATERIAL_VIEW].depth] );
		Mat4Mul( mvp, vp, g_MatStacks[MATERIAL_MODEL].stack[g_MatStacks[MATERIAL_MODEL].depth] );
		Mat4Transpose( dstColumnMajor, mvp );
	}

	// One fallback texture (flat white) that every draw samples from for now.
	// One-shot blocking upload of RGBA8 pixel data into an already-created
	// VkImage (OPTIMAL tiling, SAMPLED|TRANSFER_DST usage) via a staging
	// buffer. Shared by the fallback white texture and real per-material
	// texture uploads (TexImage2D/TexUnlock).
	bool UploadRgba8ToImage( VkImage image, uint32_t width, uint32_t height, const void *pRgba8Data )
	{
		VkDeviceSize size = (VkDeviceSize)width * height * 4;
		VkBuffer stagingBuffer = VK_NULL_HANDLE;
		VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
		if ( !VRXR_CreateBuffer( size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &stagingBuffer, &stagingMemory ) )
			return false;

		void *pData = NULL;
		vkMapMemory( VRXR_GetDevice(), stagingMemory, 0, size, 0, &pData );
		memcpy( pData, pRgba8Data, (size_t)size );
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

		VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		VkImageMemoryBarrier toDst = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
		toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		toDst.image = image;
		toDst.subresourceRange = range;
		vkCmdPipelineBarrier( cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &toDst );

		VkBufferImageCopy copyRegion = {};
		copyRegion.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		copyRegion.imageExtent = { width, height, 1 };
		vkCmdCopyBufferToImage( cmd, stagingBuffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion );

		VkImageMemoryBarrier toRead = toDst;
		toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		vkCmdPipelineBarrier( cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &toRead );

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

	VkImageView CreateImageViewRgba8( VkImage image )
	{
		VkImageViewCreateInfo viewInfo = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
		viewInfo.image = image;
		viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
		viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		VkImageView view = VK_NULL_HANDLE;
		vkCreateImageView( VRXR_GetDevice(), &viewInfo, NULL, &view );
		return view;
	}

	VkImage g_WhiteImage = VK_NULL_HANDLE;
	VkDeviceMemory g_WhiteImageMemory = VK_NULL_HANDLE;
	VkImageView g_WhiteImageView = VK_NULL_HANDLE;

	VkImageView EnsureWhiteTexture()
	{
		if ( g_WhiteImageView != VK_NULL_HANDLE )
			return g_WhiteImageView;

		if ( !VRXR_CreateImage2D( 1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TILING_OPTIMAL,
			VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &g_WhiteImage, &g_WhiteImageMemory ) )
			return VK_NULL_HANDLE;

		unsigned char whitePixel[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
		if ( !UploadRgba8ToImage( g_WhiteImage, 1, 1, whitePixel ) )
			return VK_NULL_HANDLE;

		g_WhiteImageView = CreateImageViewRgba8( g_WhiteImage );
		return g_WhiteImageView;
	}

	//-------------------------------------------------------------------------
	// Real per-material texture upload. CreateTexture()/CreateTextures() just
	// record width/height (Vulkan resources are created lazily on first
	// upload, since these get called well before the device exists - see the
	// EnsureGpuBuffers() comment on CEmptyMesh for the same pattern).
	// TexImage2D() handles immediate uploads; TexLock()/TexUnlock() handle
	// the lightmap system's lock-write-unlock pattern. Only uncompressed
	// source formats are converted - most world/prop materials use DXT
	// compression, which isn't decoded here, so those textures stay on the
	// flat white fallback for now.
	//-------------------------------------------------------------------------
	struct VulkanTexture
	{
		VkImage image = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		VkImageView view = VK_NULL_HANDLE;
		int width = 0;
		int height = 0;
		bool imageCreated = false;
		bool hasData = false;
	};

	// Handle N maps to g_Textures[N-1] (handle 0 always means "no texture") -
	// this NDK's libc++ has a broken std::vector(count[, value]) sized
	// constructor (fails to compile - a genuine toolchain bug, not
	// something in our code), so this stays default-constructed and only
	// ever grows via push_back(), matching the convention already used
	// throughout vr_xr_vulkan.cpp.
	std::vector<VulkanTexture> g_Textures;
	ShaderAPITextureHandle_t g_ModifyTextureHandle = 0;
	ShaderAPITextureHandle_t g_BoundTexture0 = 0; // stage 0 (base texture) only

	unsigned char *g_pTexLockScratch = NULL;
	size_t g_nTexLockScratchSize = 0;
	int g_nTexLockWidth = 0, g_nTexLockHeight = 0;

	bool IsValidTextureHandle( ShaderAPITextureHandle_t h )
	{
		return h > 0 && (size_t)h <= g_Textures.size();
	}

	// Only call once IsValidTextureHandle(h) is true.
	VulkanTexture &GetTexture( ShaderAPITextureHandle_t h )
	{
		return g_Textures[h - 1];
	}

	bool EnsureTextureImage( VulkanTexture &tex )
	{
		if ( tex.imageCreated )
			return true;
		if ( VRXR_GetDevice() == VK_NULL_HANDLE || tex.width <= 0 || tex.height <= 0 )
			return false;

		if ( !VRXR_CreateImage2D( tex.width, tex.height, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TILING_OPTIMAL,
			VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &tex.image, &tex.memory ) )
			return false;

		tex.view = CreateImageViewRgba8( tex.image );
		if ( tex.view == VK_NULL_HANDLE )
			return false;

		tex.imageCreated = true;
		return true;
	}

	// Converts numPixels of srcFormat data to RGBA8. Returns false (nothing
	// written) for compressed or unrecognized formats.
	bool ConvertToRgba8( ImageFormat srcFormat, const unsigned char *pSrc, unsigned char *pDst, int numPixels )
	{
		switch ( srcFormat )
		{
		case IMAGE_FORMAT_RGBA8888:
			memcpy( pDst, pSrc, (size_t)numPixels * 4 );
			return true;
		case IMAGE_FORMAT_BGRA8888:
			for ( int i = 0; i < numPixels; i++ )
			{
				pDst[i*4+0] = pSrc[i*4+2];
				pDst[i*4+1] = pSrc[i*4+1];
				pDst[i*4+2] = pSrc[i*4+0];
				pDst[i*4+3] = pSrc[i*4+3];
			}
			return true;
		case IMAGE_FORMAT_BGRX8888:
			for ( int i = 0; i < numPixels; i++ )
			{
				pDst[i*4+0] = pSrc[i*4+2];
				pDst[i*4+1] = pSrc[i*4+1];
				pDst[i*4+2] = pSrc[i*4+0];
				pDst[i*4+3] = 255;
			}
			return true;
		case IMAGE_FORMAT_RGB888:
			for ( int i = 0; i < numPixels; i++ )
			{
				pDst[i*4+0] = pSrc[i*3+0];
				pDst[i*4+1] = pSrc[i*3+1];
				pDst[i*4+2] = pSrc[i*3+2];
				pDst[i*4+3] = 255;
			}
			return true;
		case IMAGE_FORMAT_BGR888:
			for ( int i = 0; i < numPixels; i++ )
			{
				pDst[i*4+0] = pSrc[i*3+2];
				pDst[i*4+1] = pSrc[i*3+1];
				pDst[i*4+2] = pSrc[i*3+0];
				pDst[i*4+3] = 255;
			}
			return true;
		case IMAGE_FORMAT_I8:
			for ( int i = 0; i < numPixels; i++ )
			{
				pDst[i*4+0] = pDst[i*4+1] = pDst[i*4+2] = pSrc[i];
				pDst[i*4+3] = 255;
			}
			return true;
		case IMAGE_FORMAT_IA88:
			for ( int i = 0; i < numPixels; i++ )
			{
				pDst[i*4+0] = pDst[i*4+1] = pDst[i*4+2] = pSrc[i*2+0];
				pDst[i*4+3] = pSrc[i*2+1];
			}
			return true;
		case IMAGE_FORMAT_A8:
			for ( int i = 0; i < numPixels; i++ )
			{
				pDst[i*4+0] = pDst[i*4+1] = pDst[i*4+2] = 255;
				pDst[i*4+3] = pSrc[i];
			}
			return true;
		default:
			return false;
		}
	}
}
#endif


//-----------------------------------------------------------------------------
// The empty mesh
//-----------------------------------------------------------------------------
class CEmptyMesh : public IMesh
{
public:
	CEmptyMesh( bool bIsDynamic );
	virtual ~CEmptyMesh();

	// FIXME: Make this work! Unsupported methods of IIndexBuffer + IVertexBuffer
	virtual bool Lock( int nMaxIndexCount, bool bAppend, IndexDesc_t& desc );
	virtual void Unlock( int nWrittenIndexCount, IndexDesc_t& desc );
	virtual void ModifyBegin( bool bReadOnly, int nFirstIndex, int nIndexCount, IndexDesc_t& desc );
	virtual void ModifyEnd( IndexDesc_t& desc );
	virtual void Spew( int nIndexCount, const IndexDesc_t & desc );
	virtual void ValidateData( int nIndexCount, const IndexDesc_t &desc );
	virtual bool Lock( int nVertexCount, bool bAppend, VertexDesc_t &desc );
	virtual void Unlock( int nVertexCount, VertexDesc_t &desc );
	virtual void Spew( int nVertexCount, const VertexDesc_t &desc );
	virtual void ValidateData( int nVertexCount, const VertexDesc_t & desc );
	virtual bool IsDynamic() const { return m_bIsDynamic; }
	virtual void BeginCastBuffer( VertexFormat_t format ) {}
	virtual void BeginCastBuffer( MaterialIndexFormat_t format ) {}
	virtual void EndCastBuffer( ) {}
	virtual int GetRoomRemaining() const { return 0; }
	virtual MaterialIndexFormat_t IndexFormat() const { return MATERIAL_INDEX_FORMAT_UNKNOWN; }

	void LockMesh( int numVerts, int numIndices, MeshDesc_t& desc );
	void UnlockMesh( int numVerts, int numIndices, MeshDesc_t& desc );

	void ModifyBeginEx( bool bReadOnly, int firstVertex, int numVerts, int firstIndex, int numIndices, MeshDesc_t& desc );
	void ModifyBegin( int firstVertex, int numVerts, int firstIndex, int numIndices, MeshDesc_t& desc );
	void ModifyEnd( MeshDesc_t& desc );

	// returns the # of vertices (static meshes only)
	int  VertexCount() const;

	// Sets the primitive type
	void SetPrimitiveType( MaterialPrimitiveType_t type );
	 
	// Draws the entire mesh
	void Draw(int firstIndex, int numIndices);

	void Draw(CPrimList *pPrims, int nPrims);

	// Copy verts and/or indices to a mesh builder. This only works for temp meshes!
	virtual void CopyToMeshBuilder( 
		int iStartVert,		// Which vertices to copy.
		int nVerts, 
		int iStartIndex,	// Which indices to copy.
		int nIndices, 
		int indexOffset,	// This is added to each index.
		CMeshBuilder &builder );

	// Spews the mesh data
	void Spew( int numVerts, int numIndices, const MeshDesc_t & desc );

	void ValidateData( int numVerts, int numIndices, const MeshDesc_t & desc );

	// gets the associated material
	IMaterial* GetMaterial();

	void SetColorMesh( IMesh *pColorMesh, int nVertexOffset )
	{
	}


	virtual int IndexCount() const
	{
		return 0;
	}

	virtual void SetFlexMesh( IMesh *pMesh, int nVertexOffset ) {}

	virtual void DisableFlexMesh() {}

	virtual void MarkAsDrawn() {}

	virtual unsigned ComputeMemoryUsed() { return 0; }

	virtual VertexFormat_t GetVertexFormat() const { return VERTEX_POSITION; }

	virtual IMesh *GetMesh()
	{
		return this;
	}

private:
	enum
	{
		VERTEX_BUFFER_SIZE = 1024 * 1024,
		INDEX_BUFFER_COUNT = 65536
	};

	unsigned char* m_pVertexMemory;
	bool m_bIsDynamic;

#if defined( ANDROID )
	bool EnsureGpuBuffers();
	void UploadToGpu( int numVerts, int numIndices );

	unsigned short *m_pIndexMemory;
	float m_DummyAttrib[4];

	// Allocated once (lazily, on first use after the Vulkan device exists)
	// and persistently mapped for the lifetime of this object, rather than
	// a fresh VkBuffer/vkAllocateMemory per Lock/Unlock cycle - meshes here
	// get re-locked and re-uploaded constantly (every world surface during
	// level load, every dynamic/UI draw every frame), and allocating +
	// freeing real device memory that often is both slow enough to look
	// like a hang and, if ever deferred instead, enough to blow through the
	// device's live allocation budget (VK_ERROR_OUT_OF_DEVICE_MEMORY).
	bool m_bGpuBuffersReady;
	VkBuffer m_hVB;
	VkDeviceMemory m_hVBMem;
	void *m_pMappedVB;
	VkBuffer m_hIB;
	VkDeviceMemory m_hIBMem;
	void *m_pMappedIB;
	int m_nPendingVerts;
	int m_nPendingIndices;
	int m_nDrawIndexCount;
#endif
};


//-----------------------------------------------------------------------------
// The empty shader shadow
//-----------------------------------------------------------------------------
class CShaderShadowEmpty : public IShaderShadow
{
public:
	CShaderShadowEmpty();
	virtual ~CShaderShadowEmpty();

	// Sets the default *shadow* state
	void SetDefaultState();

	// Methods related to depth buffering
	void DepthFunc( ShaderDepthFunc_t depthFunc );
	void EnableDepthWrites( bool bEnable );
	void EnableDepthTest( bool bEnable );
	void EnablePolyOffset( PolygonOffsetMode_t nOffsetMode );

	// Suppresses/activates color writing 
	void EnableColorWrites( bool bEnable );
	void EnableAlphaWrites( bool bEnable );

	// Methods related to alpha blending
	void EnableBlending( bool bEnable );
	void BlendFunc( ShaderBlendFactor_t srcFactor, ShaderBlendFactor_t dstFactor );

	// Alpha testing
	void EnableAlphaTest( bool bEnable );
	void AlphaFunc( ShaderAlphaFunc_t alphaFunc, float alphaRef /* [0-1] */ );

	// Wireframe/filled polygons
	void PolyMode( ShaderPolyModeFace_t face, ShaderPolyMode_t polyMode );

	// Back face culling
	void EnableCulling( bool bEnable );
	
	// constant color + transparency
	void EnableConstantColor( bool bEnable );

	// Indicates the vertex format for use with a vertex shader
	// The flags to pass in here come from the VertexFormatFlags_t enum
	// If pTexCoordDimensions is *not* specified, we assume all coordinates
	// are 2-dimensional
	void VertexShaderVertexFormat( unsigned int nFlags, 
		int nTexCoordCount, int* pTexCoordDimensions, int nUserDataSize );
	
	// Indicates we're going to light the model
	void EnableLighting( bool bEnable );
	void EnableSpecular( bool bEnable );

	// vertex blending
	void EnableVertexBlend( bool bEnable );

	// per texture unit stuff
	void OverbrightValue( TextureStage_t stage, float value );
	void EnableTexture( Sampler_t stage, bool bEnable );
	void EnableTexGen( TextureStage_t stage, bool bEnable );
	void TexGen( TextureStage_t stage, ShaderTexGenParam_t param );

	// alternate method of specifying per-texture unit stuff, more flexible and more complicated
	// Can be used to specify different operation per channel (alpha/color)...
	void EnableCustomPixelPipe( bool bEnable );
	void CustomTextureStages( int stageCount );
	void CustomTextureOperation( TextureStage_t stage, ShaderTexChannel_t channel, 
		ShaderTexOp_t op, ShaderTexArg_t arg1, ShaderTexArg_t arg2 );

	// indicates what per-vertex data we're providing
	void DrawFlags( unsigned int drawFlags );

	// A simpler method of dealing with alpha modulation
	void EnableAlphaPipe( bool bEnable );
	void EnableConstantAlpha( bool bEnable );
	void EnableVertexAlpha( bool bEnable );
	void EnableTextureAlpha( TextureStage_t stage, bool bEnable );

	// GR - Separate alpha blending
	void EnableBlendingSeparateAlpha( bool bEnable );
	void BlendFuncSeparateAlpha( ShaderBlendFactor_t srcFactor, ShaderBlendFactor_t dstFactor );

	// Sets the vertex and pixel shaders
	void SetVertexShader( const char *pFileName, int vshIndex );
	void SetPixelShader( const char *pFileName, int pshIndex );

	// Convert from linear to gamma color space on writes to frame buffer.
	void EnableSRGBWrite( bool bEnable )
	{
	}

	void EnableSRGBRead( Sampler_t stage, bool bEnable )
	{
	}

	virtual void FogMode( ShaderFogMode_t fogMode )
	{
	}

	virtual void DisableFogGammaCorrection( bool bDisable )
	{
	}

	virtual void SetDiffuseMaterialSource( ShaderMaterialSource_t materialSource )
	{
	}

	virtual void SetMorphFormat( MorphFormat_t flags )
	{
	}

	virtual void EnableStencil( bool bEnable )
	{
	}
	virtual void StencilFunc( ShaderStencilFunc_t stencilFunc )
	{
	}
	virtual void StencilPassOp( ShaderStencilOp_t stencilOp )
	{
	}
	virtual void StencilFailOp( ShaderStencilOp_t stencilOp )
	{
	}
	virtual void StencilDepthFailOp( ShaderStencilOp_t stencilOp )
	{
	}
	virtual void StencilReference( int nReference )
	{
	}
	virtual void StencilMask( int nMask )
	{
	}
	virtual void StencilWriteMask( int nMask )
	{
	}

	virtual void ExecuteCommandBuffer( uint8 *pBuf ) 
	{
	}
	// Alpha to coverage
	void EnableAlphaToCoverage( bool bEnable );
	
	virtual void SetShadowDepthFiltering( Sampler_t stage )
	{
	}

	virtual void BlendOp( ShaderBlendOp_t blendOp ) {}
	virtual void BlendOpSeparateAlpha( ShaderBlendOp_t blendOp ) {}

	bool m_IsTranslucent;
	bool m_IsAlphaTested;
	bool m_bIsDepthWriteEnabled;
	bool m_bUsesVertexAndPixelShaders;
};


//-----------------------------------------------------------------------------
// The DX8 implementation of the shader device
//-----------------------------------------------------------------------------
class CShaderDeviceEmpty : public IShaderDevice
{
public:
	CShaderDeviceEmpty() : m_DynamicMesh( true ), m_Mesh( false ) {}

	// Methods of IShaderDevice
	virtual int GetCurrentAdapter() const { return 0; }
	virtual bool IsUsingGraphics() const { return false; }
	virtual void SpewDriverInfo() const;
	virtual ImageFormat GetBackBufferFormat() const { return IMAGE_FORMAT_RGB888; }
	virtual void GetBackBufferDimensions( int& width, int& height ) const;
	virtual int  StencilBufferBits() const { return 0; }
	virtual bool IsAAEnabled() const { return false; }
	virtual void Present( )
	{
#if defined( ANDROID )
		// SetMode()/its CreateInterfaceFn indirection is never actually
		// called in this build's real init flow - materialsystem fetches
		// IShaderDevice straight from the EXPOSE_SINGLE_INTERFACE_GLOBALVAR
		// registration below. Present() is confirmed-called every frame
		// (verified on device), so that's where the VR session actually
		// gets created, lazily, on first use.
		static bool s_bTriedVrInit = false;
		if ( !s_bTriedVrInit )
		{
			s_bTriedVrInit = true;

			const char *pInstanceHex = getenv( "HL2VR_XR_INSTANCE" );
			const char *pSystemIdHex = getenv( "HL2VR_XR_SYSTEM_ID" );
			if ( pInstanceHex && pSystemIdHex )
			{
				XrInstance instance = (XrInstance)(uintptr_t)strtoull( pInstanceHex, NULL, 16 );
				XrSystemId systemId = (XrSystemId)strtoull( pSystemIdHex, NULL, 16 );
				VRXR_Init( instance, systemId );
			}
			else
			{
				Warning( "shaderapivulkan: HL2VR_XR_INSTANCE/HL2VR_XR_SYSTEM_ID not set - no VR session\n" );
			}
		}

		// Safety net in case ClearBuffers() wasn't hit this frame for some
		// reason - VRXR_BeginSceneIfNeeded() is idempotent.
		VRXR_BeginSceneIfNeeded();
		VRXR_EndSceneAndPresent();
#endif
	}
	virtual void GetWindowSize( int &width, int &height ) const;
	virtual bool AddView( void* hwnd );
	virtual void RemoveView( void* hwnd );
	virtual void SetView( void* hwnd );
	virtual void ReleaseResources();
	virtual void ReacquireResources();
	virtual IMesh* CreateStaticMesh( VertexFormat_t fmt, const char *pTextureBudgetGroup, IMaterial * pMaterial = NULL );
	virtual void DestroyStaticMesh( IMesh* mesh );
	virtual IShaderBuffer* CompileShader( const char *pProgram, size_t nBufLen, const char *pShaderVersion ) { return NULL; }
	virtual VertexShaderHandle_t CreateVertexShader( IShaderBuffer* pShaderBuffer ) { return VERTEX_SHADER_HANDLE_INVALID; }
	virtual void DestroyVertexShader( VertexShaderHandle_t hShader ) {}
	virtual GeometryShaderHandle_t CreateGeometryShader( IShaderBuffer* pShaderBuffer ) { return GEOMETRY_SHADER_HANDLE_INVALID; }
	virtual void DestroyGeometryShader( GeometryShaderHandle_t hShader ) {}
	virtual PixelShaderHandle_t CreatePixelShader( IShaderBuffer* pShaderBuffer ) { return PIXEL_SHADER_HANDLE_INVALID; }
	virtual void DestroyPixelShader( PixelShaderHandle_t hShader ) {}
	virtual IVertexBuffer *CreateVertexBuffer( ShaderBufferType_t type, VertexFormat_t fmt, int nVertexCount, const char *pBudgetGroup );
	virtual void DestroyVertexBuffer( IVertexBuffer *pVertexBuffer );
	virtual IIndexBuffer *CreateIndexBuffer( ShaderBufferType_t bufferType, MaterialIndexFormat_t fmt, int nIndexCount, const char *pBudgetGroup );
	virtual void DestroyIndexBuffer( IIndexBuffer *pIndexBuffer );
	virtual IVertexBuffer *GetDynamicVertexBuffer( int streamID, VertexFormat_t vertexFormat, bool bBuffered );
	virtual IIndexBuffer *GetDynamicIndexBuffer( MaterialIndexFormat_t fmt, bool bBuffered );
	virtual void SetHardwareGammaRamp( float fGamma, float fGammaTVRangeMin, float fGammaTVRangeMax, float fGammaTVExponent, bool bTVEnabled ) {}
	virtual void EnableNonInteractiveMode( MaterialNonInteractiveMode_t mode, ShaderNonInteractiveInfo_t *pInfo ) {}
	virtual void RefreshFrontBufferNonInteractive( ) {}
	virtual void HandleThreadEvent( uint32 threadEvent ) {}

#ifdef DX_TO_GL_ABSTRACTION
	virtual void DoStartupShaderPreloading( void ) {}
#endif

	virtual char *GetDisplayDeviceName() OVERRIDE { return ""; }

private:
	CEmptyMesh m_Mesh;
	CEmptyMesh m_DynamicMesh;
};

static CShaderDeviceEmpty s_ShaderDeviceEmpty;

// FIXME: Remove; it's for backward compat with the materialsystem only for now
EXPOSE_SINGLE_INTERFACE_GLOBALVAR( CShaderDeviceEmpty, IShaderDevice, 
								  SHADER_DEVICE_INTERFACE_VERSION, s_ShaderDeviceEmpty )


//-----------------------------------------------------------------------------
// The DX8 implementation of the shader device
//-----------------------------------------------------------------------------
class CShaderDeviceMgrEmpty : public IShaderDeviceMgr
{
public:
	// Methods of IAppSystem
	virtual bool Connect( CreateInterfaceFn factory );
	virtual void Disconnect();
	virtual void *QueryInterface( const char *pInterfaceName );
	virtual InitReturnVal_t Init();
	virtual void Shutdown();

public:
	// Methods of IShaderDeviceMgr
	virtual int	 GetAdapterCount() const;
	virtual void GetAdapterInfo( int adapter, MaterialAdapterInfo_t& info ) const;
	virtual bool GetRecommendedConfigurationInfo( int nAdapter, int nDXLevel, KeyValues *pKeyValues );
	virtual int	 GetModeCount( int adapter ) const;
	virtual void GetModeInfo( ShaderDisplayMode_t *pInfo, int nAdapter, int mode ) const;
	virtual void GetCurrentModeInfo( ShaderDisplayMode_t* pInfo, int nAdapter ) const;
	virtual bool SetAdapter( int nAdapter, int nFlags );
	virtual CreateInterfaceFn SetMode( void *hWnd, int nAdapter, const ShaderDeviceInfo_t& mode );
	virtual void AddModeChangeCallback( ShaderModeChangeCallbackFunc_t func ) {}
	virtual void RemoveModeChangeCallback( ShaderModeChangeCallbackFunc_t func ) {}
};

static CShaderDeviceMgrEmpty s_ShaderDeviceMgrEmpty;

EXPOSE_SINGLE_INTERFACE_GLOBALVAR( CShaderDeviceMgrEmpty, IShaderDeviceMgr, 
								  SHADER_DEVICE_MGR_INTERFACE_VERSION, s_ShaderDeviceMgrEmpty )


//-----------------------------------------------------------------------------
// The DX8 implementation of the shader API
//-----------------------------------------------------------------------------
class CShaderAPIEmpty : public IShaderAPI, public IHardwareConfigInternal, public IDebugTextureInfo
{
public:
	// constructor, destructor
	CShaderAPIEmpty( );
	virtual ~CShaderAPIEmpty();

	// IDebugTextureInfo implementation.
public:

	virtual bool IsDebugTextureListFresh( int numFramesAllowed = 1 ) { return false; }
	virtual bool SetDebugTextureRendering( bool bEnable ) { return false; }
	virtual void EnableDebugTextureList( bool bEnable ) {}
	virtual void EnableGetAllTextures( bool bEnable ) {}
	virtual KeyValues* GetDebugTextureList() { return NULL; }
	virtual int GetTextureMemoryUsed( TextureMemoryType eTextureMemory ) { return 0; }

	// Methods of IShaderDynamicAPI
	virtual void GetBackBufferDimensions( int& width, int& height ) const
	{
		s_ShaderDeviceEmpty.GetBackBufferDimensions( width, height );
	}
	virtual void GetCurrentColorCorrection( ShaderColorCorrectionInfo_t* pInfo )
	{
		pInfo->m_bIsEnabled = false;
		pInfo->m_nLookupCount = 0;
		pInfo->m_flDefaultWeight = 0.0f;
	}


	// Methods of IShaderAPI
public:
	virtual void SetViewports( int nCount, const ShaderViewport_t* pViewports );
	virtual int GetViewports( ShaderViewport_t* pViewports, int nMax ) const;
	virtual void ClearBuffers( bool bClearColor, bool bClearDepth, bool bClearStencil, int renderTargetWidth, int renderTargetHeight );
	virtual void ClearColor3ub( unsigned char r, unsigned char g, unsigned char b );
	virtual void ClearColor4ub( unsigned char r, unsigned char g, unsigned char b, unsigned char a );
	virtual void BindVertexShader( VertexShaderHandle_t hVertexShader ) {}
	virtual void BindGeometryShader( GeometryShaderHandle_t hGeometryShader ) {}
	virtual void BindPixelShader( PixelShaderHandle_t hPixelShader ) {}
	virtual void SetRasterState( const ShaderRasterState_t& state ) {}
	virtual void MarkUnusedVertexFields( unsigned int nFlags, int nTexCoordCount, bool *pUnusedTexCoords ) {}
	virtual bool OwnGPUResources( bool bEnable ) { return false; }

	virtual bool DoRenderTargetsNeedSeparateDepthBuffer() const;

	// Used to clear the transition table when we know it's become invalid.
	void ClearSnapshots();

	// Sets the mode...
	bool SetMode( void* hwnd, int nAdapter, const ShaderDeviceInfo_t &info )
	{
		return true;
	}

	void ChangeVideoMode( const ShaderDeviceInfo_t &info )
	{
	}

	// Called when the dx support level has changed
	virtual void DXSupportLevelChanged() {}

	virtual void EnableUserClipTransformOverride( bool bEnable ) {}
	virtual void UserClipTransform( const VMatrix &worldToView ) {}

	// Sets the default *dynamic* state
	void SetDefaultState( );

	// Returns the snapshot id for the shader state
	StateSnapshot_t	 TakeSnapshot( );

	// Returns true if the state snapshot is transparent
	bool IsTranslucent( StateSnapshot_t id ) const;
	bool IsAlphaTested( StateSnapshot_t id ) const;
	bool UsesVertexAndPixelShaders( StateSnapshot_t id ) const;
	virtual bool IsDepthWriteEnabled( StateSnapshot_t id ) const;

	// Gets the vertex format for a set of snapshot ids
	VertexFormat_t ComputeVertexFormat( int numSnapshots, StateSnapshot_t* pIds ) const;

	// Gets the vertex format for a set of snapshot ids
	VertexFormat_t ComputeVertexUsage( int numSnapshots, StateSnapshot_t* pIds ) const;

	// Begins a rendering pass that uses a state snapshot
	void BeginPass( StateSnapshot_t snapshot  );

	// Uses a state snapshot
	void UseSnapshot( StateSnapshot_t snapshot );

	// Use this to get the mesh builder that allows us to modify vertex data
	CMeshBuilder* GetVertexModifyBuilder();

	// Sets the color to modulate by
	void Color3f( float r, float g, float b );
	void Color3fv( float const* pColor );
	void Color4f( float r, float g, float b, float a );
	void Color4fv( float const* pColor );

	// Faster versions of color
	void Color3ub( unsigned char r, unsigned char g, unsigned char b );
	void Color3ubv( unsigned char const* rgb );
	void Color4ub( unsigned char r, unsigned char g, unsigned char b, unsigned char a );
	void Color4ubv( unsigned char const* rgba );

	// Sets the lights
	void SetLight( int lightNum, const LightDesc_t& desc );
	void SetLightingOrigin( Vector vLightingOrigin );
	void SetAmbientLight( float r, float g, float b );
	void SetAmbientLightCube( Vector4D cube[6] );

	// Get the lights
	int GetMaxLights( void ) const;
	const LightDesc_t& GetLight( int lightNum ) const;

	// Render state for the ambient light cube (vertex shaders)
	void SetVertexShaderStateAmbientLightCube();
	void SetPixelShaderStateAmbientLightCube( int pshReg, bool bForceToBlack = false )
	{
	}

	float GetAmbientLightCubeLuminance(void)
	{
		return 0.0f;
	}

	void SetSkinningMatrices();

	// Lightmap texture binding
	void BindLightmap( TextureStage_t stage );
	void BindLightmapAlpha( TextureStage_t stage )
	{
	}
	void BindBumpLightmap( TextureStage_t stage );
	void BindFullbrightLightmap( TextureStage_t stage );
	void BindWhite( TextureStage_t stage );
	void BindBlack( TextureStage_t stage );
	void BindGrey( TextureStage_t stage );
	void BindFBTexture( TextureStage_t stage, int textureIdex );
	void CopyRenderTargetToTexture( ShaderAPITextureHandle_t texID )
	{
	}

	void CopyRenderTargetToTextureEx( ShaderAPITextureHandle_t texID, int nRenderTargetID, Rect_t *pSrcRect, Rect_t *pDstRect )
	{
	}

	void CopyTextureToRenderTargetEx( int nRenderTargetID, ShaderAPITextureHandle_t textureHandle, Rect_t *pSrcRect, Rect_t *pDstRect )
	{
	}

	// Special system flat normal map binding.
	void BindFlatNormalMap( TextureStage_t stage );
	void BindNormalizationCubeMap( TextureStage_t stage );
	void BindSignedNormalizationCubeMap( TextureStage_t stage );

	// Set the number of bone weights
	void SetNumBoneWeights( int numBones );
	void EnableHWMorphing( bool bEnable );

	// Flushes any primitives that are buffered
	void FlushBufferedPrimitives();

	// Gets the dynamic mesh; note that you've got to render the mesh
	// before calling this function a second time. Clients should *not*
	// call DestroyStaticMesh on the mesh returned by this call.
	IMesh* GetDynamicMesh( IMaterial* pMaterial, int nHWSkinBoneCount, bool buffered, IMesh* pVertexOverride, IMesh* pIndexOverride );
	IMesh* GetDynamicMeshEx( IMaterial* pMaterial, VertexFormat_t fmt, int nHWSkinBoneCount, bool buffered, IMesh* pVertexOverride, IMesh* pIndexOverride );

	IMesh* GetFlexMesh();

	// Renders a single pass of a material
	void RenderPass( int nPass, int nPassCount );

	// stuff related to matrix stacks
	void MatrixMode( MaterialMatrixMode_t matrixMode );
	void PushMatrix();
	void PopMatrix();
	void LoadMatrix( float *m );
	void LoadBoneMatrix( int boneIndex, const float *m ) {}
	void MultMatrix( float *m );
	void MultMatrixLocal( float *m );
	void GetMatrix( MaterialMatrixMode_t matrixMode, float *dst );
	void LoadIdentity( void );
	void LoadCameraToWorld( void );
	void Ortho( double left, double top, double right, double bottom, double zNear, double zFar );
	void PerspectiveX( double fovx, double aspect, double zNear, double zFar );
	void PerspectiveOffCenterX( double fovx, double aspect, double zNear, double zFar, double bottom, double top, double left, double right );
	void PickMatrix( int x, int y, int width, int height );
	void Rotate( float angle, float x, float y, float z );
	void Translate( float x, float y, float z );
	void Scale( float x, float y, float z );
	void ScaleXY( float x, float y );

	// Fog methods...
	void FogMode( MaterialFogMode_t fogMode );
	void FogStart( float fStart );
	void FogEnd( float fEnd );
	void SetFogZ( float fogZ );
	void FogMaxDensity( float flMaxDensity );
	void GetFogDistances( float *fStart, float *fEnd, float *fFogZ );
	void FogColor3f( float r, float g, float b );
	void FogColor3fv( float const* rgb );
	void FogColor3ub( unsigned char r, unsigned char g, unsigned char b );
	void FogColor3ubv( unsigned char const* rgb );

	virtual void SceneFogColor3ub( unsigned char r, unsigned char g, unsigned char b );
	virtual void SceneFogMode( MaterialFogMode_t fogMode );
	virtual void GetSceneFogColor( unsigned char *rgb );
	virtual MaterialFogMode_t GetSceneFogMode( );
	virtual int GetPixelFogCombo( );

	void SetHeightClipZ( float z ); 
	void SetHeightClipMode( enum MaterialHeightClipMode_t heightClipMode ); 

	void SetClipPlane( int index, const float *pPlane );
	void EnableClipPlane( int index, bool bEnable );

	void SetFastClipPlane( const float *pPlane );
	void EnableFastClip( bool bEnable );
	
	// We use smaller dynamic VBs during level transitions, to free up memory
	virtual int  GetCurrentDynamicVBSize( void );
	virtual void DestroyVertexBuffers( bool bExitingLevel = false );

	// Sets the vertex and pixel shaders
	void SetVertexShaderIndex( int vshIndex );
	void SetPixelShaderIndex( int pshIndex );

	// Sets the constant register for vertex and pixel shaders
	void SetVertexShaderConstant( int var, float const* pVec, int numConst = 1, bool bForce = false );
	void SetBooleanVertexShaderConstant( int var, BOOL const* pVec, int numConst = 1, bool bForce = false );
	void SetIntegerVertexShaderConstant( int var, int const* pVec, int numConst = 1, bool bForce = false );
	void SetPixelShaderConstant( int var, float const* pVec, int numConst = 1, bool bForce = false );
	void SetBooleanPixelShaderConstant( int var, BOOL const* pVec, int numBools = 1, bool bForce = false );
	void SetIntegerPixelShaderConstant( int var, int const* pVec, int numIntVecs = 1, bool bForce = false );

	void InvalidateDelayedShaderConstants( void );

	// Gamma<->Linear conversions according to the video hardware we're running on
	float GammaToLinear_HardwareSpecific( float fGamma ) const;
	float LinearToGamma_HardwareSpecific( float fLinear ) const;

	//Set's the linear->gamma conversion textures to use for this hardware for both srgb writes enabled and disabled(identity)
	void SetLinearToGammaConversionTextures( ShaderAPITextureHandle_t hSRGBWriteEnabledTexture, ShaderAPITextureHandle_t hIdentityTexture );

	// Cull mode
	void CullMode( MaterialCullMode_t cullMode );

	// Force writes only when z matches. . . useful for stenciling things out
	// by rendering the desired Z values ahead of time.
	void ForceDepthFuncEquals( bool bEnable );

	// Forces Z buffering on or off
	void OverrideDepthEnable( bool bEnable, bool bDepthEnable );
	void OverrideAlphaWriteEnable( bool bOverrideEnable, bool bAlphaWriteEnable );
	void OverrideColorWriteEnable( bool bOverrideEnable, bool bColorWriteEnable );

	// Sets the shade mode
	void ShadeMode( ShaderShadeMode_t mode );

	// Binds a particular material to render with
	void Bind( IMaterial* pMaterial );

	// Returns the nearest supported format
	ImageFormat GetNearestSupportedFormat( ImageFormat fmt, bool bFilteringRequired = true ) const;
 	ImageFormat GetNearestRenderTargetFormat( ImageFormat fmt ) const;

	// Sets the texture state
	void BindTexture( Sampler_t stage, ShaderAPITextureHandle_t textureHandle );

	void SetRenderTarget( ShaderAPITextureHandle_t colorTextureHandle, ShaderAPITextureHandle_t depthTextureHandle )
	{
	}

	void SetRenderTargetEx( int nRenderTargetID, ShaderAPITextureHandle_t colorTextureHandle, ShaderAPITextureHandle_t depthTextureHandle )
	{
	}

	// Indicates we're going to be modifying this texture
	// TexImage2D, TexSubImage2D, TexWrap, TexMinFilter, and TexMagFilter
	// all use the texture specified by this function.
	void ModifyTexture( ShaderAPITextureHandle_t textureHandle );

	// Texture management methods
	void TexImage2D( int level, int cubeFace, ImageFormat dstFormat, int zOffset, int width, int height, 
							 ImageFormat srcFormat, bool bSrcIsTiled, void *imageData );
	void TexSubImage2D( int level, int cubeFace, int xOffset, int yOffset, int zOffset, int width, int height,
							 ImageFormat srcFormat, int srcStride, bool bSrcIsTiled, void *imageData );

	void TexImageFromVTF( IVTFTexture *pVTF, int iVTFFrame );

	bool TexLock( int level, int cubeFaceID, int xOffset, int yOffset, 
									int width, int height, CPixelWriter& writer );
	void TexUnlock( );
	
	// These are bound to the texture, not the texture environment
	void TexMinFilter( ShaderTexFilterMode_t texFilterMode );
	void TexMagFilter( ShaderTexFilterMode_t texFilterMode );
	void TexWrap( ShaderTexCoordComponent_t coord, ShaderTexWrapMode_t wrapMode );
	void TexSetPriority( int priority );	

	ShaderAPITextureHandle_t CreateTexture( 
		int width, 
		int height,
		int depth,
		ImageFormat dstImageFormat, 
		int numMipLevels, 
		int numCopies, 
		int flags, 
		const char *pDebugName,
		const char *pTextureGroupName );
	// Create a multi-frame texture (equivalent to calling "CreateTexture" multiple times, but more efficient)
	void CreateTextures( 
		ShaderAPITextureHandle_t *pHandles,
		int count,
		int width, 
		int height,
		int depth,
		ImageFormat dstImageFormat, 
		int numMipLevels, 
		int numCopies, 
		int flags, 
		const char *pDebugName,
		const char *pTextureGroupName );
	ShaderAPITextureHandle_t CreateDepthTexture( ImageFormat renderFormat, int width, int height, const char *pDebugName, bool bTexture );
	void DeleteTexture( ShaderAPITextureHandle_t textureHandle );
	bool IsTexture( ShaderAPITextureHandle_t textureHandle );
	bool IsTextureResident( ShaderAPITextureHandle_t textureHandle );

	// stuff that isn't to be used from within a shader
	void ClearBuffersObeyStencil( bool bClearColor, bool bClearDepth );
	void ClearBuffersObeyStencilEx( bool bClearColor, bool bClearAlpha, bool bClearDepth );
	void PerformFullScreenStencilOperation( void );
	void ReadPixels( int x, int y, int width, int height, unsigned char *data, ImageFormat dstFormat );
	virtual void ReadPixels( Rect_t *pSrcRect, Rect_t *pDstRect, unsigned char *data, ImageFormat dstFormat, int nDstStride );

	// Selection mode methods
	int SelectionMode( bool selectionMode );
	void SelectionBuffer( unsigned int* pBuffer, int size );
	void ClearSelectionNames( );
	void LoadSelectionName( int name );
	void PushSelectionName( int name );
	void PopSelectionName();

	void FlushHardware();
	void ResetRenderState( bool bFullReset = true );

	void SetScissorRect( const int nLeft, const int nTop, const int nRight, const int nBottom, const bool bEnableScissor );

	// Can we download textures?
	virtual bool CanDownloadTextures() const;

	// Board-independent calls, here to unify how shaders set state
	// Implementations should chain back to IShaderUtil->BindTexture(), etc.

	// Use this to begin and end the frame
	void BeginFrame();
	void EndFrame();

	// returns current time
	double CurrentTime() const;

	// Get the current camera position in world space.
	void GetWorldSpaceCameraPosition( float * pPos ) const;

	// Members of IMaterialSystemHardwareConfig
	bool HasDestAlphaBuffer() const;
	bool HasStencilBuffer() const;
	virtual int  MaxViewports() const;
	virtual void OverrideStreamOffsetSupport( bool bOverrideEnabled, bool bEnableSupport ) {}
	virtual int  GetShadowFilterMode() const;
	int  StencilBufferBits() const;
	int	 GetFrameBufferColorDepth() const;
	int  GetSamplerCount() const;
	bool HasSetDeviceGammaRamp() const;
	bool SupportsCompressedTextures() const;
	VertexCompressionType_t SupportsCompressedVertices() const;
	bool SupportsVertexAndPixelShaders() const;
	bool SupportsPixelShaders_1_4() const;
	bool SupportsPixelShaders_2_0() const;
	bool SupportsPixelShaders_2_b() const;
	bool ActuallySupportsPixelShaders_2_b() const;
	bool SupportsStaticControlFlow() const;
	bool SupportsVertexShaders_2_0() const;
	bool SupportsShaderModel_3_0() const;
	int  MaximumAnisotropicLevel() const;
	int  MaxTextureWidth() const;
	int  MaxTextureHeight() const;
	int  MaxTextureAspectRatio() const;
	int  GetDXSupportLevel() const;
	const char *GetShaderDLLName() const
	{
		return "UNKNOWN";
	}
	int	 TextureMemorySize() const;
	bool SupportsOverbright() const;
	bool SupportsCubeMaps() const;
	bool SupportsMipmappedCubemaps() const;
	bool SupportsNonPow2Textures() const;
	int  GetTextureStageCount() const;
	int	 NumVertexShaderConstants() const;
	int	 NumBooleanVertexShaderConstants() const;
	int	 NumIntegerVertexShaderConstants() const;
	int	 NumPixelShaderConstants() const;
	int	 MaxNumLights() const;
	bool SupportsHardwareLighting() const;
	int	 MaxBlendMatrices() const;
	int	 MaxBlendMatrixIndices() const;
	int	 MaxVertexShaderBlendMatrices() const;
	int	 MaxUserClipPlanes() const;
	bool UseFastClipping() const
	{
		return false;
	}
	bool SpecifiesFogColorInLinearSpace() const;
	virtual bool SupportsSRGB() const;
	virtual bool FakeSRGBWrite() const;
	virtual bool CanDoSRGBReadFromRTs() const;
	virtual bool SupportsGLMixedSizeTargets() const;

	const char *GetHWSpecificShaderDLLName() const;
	bool NeedsAAClamp() const
	{
		return false;
	}
	bool SupportsSpheremapping() const;
	virtual int MaxHWMorphBatchCount() const { return 0; }

	// This is the max dx support level supported by the card
	virtual int	 GetMaxDXSupportLevel() const;

	bool ReadPixelsFromFrontBuffer() const;
	bool PreferDynamicTextures() const;
	virtual bool PreferReducedFillrate() const;
	bool HasProjectedBumpEnv() const;
	void ForceHardwareSync( void );
	
	int GetCurrentNumBones( void ) const;
	bool IsHWMorphingEnabled( void ) const;
	int GetCurrentLightCombo( void ) const;
	void GetDX9LightState( LightState_t *state ) const;
	MaterialFogMode_t GetCurrentFogType( void ) const;

	void RecordString( const char *pStr );

	void EvictManagedResources();

	void SetTextureTransformDimension( TextureStage_t textureStage, int dimension, bool projected );
	void DisableTextureTransform( TextureStage_t textureStage )
	{
	}
	void SetBumpEnvMatrix( TextureStage_t textureStage, float m00, float m01, float m10, float m11 );

	// Gets the lightmap dimensions
	virtual void GetLightmapDimensions( int *w, int *h );

	virtual void SyncToken( const char *pToken );

	// Setup standard vertex shader constants (that don't change)
	// This needs to be called anytime that overbright changes.
	virtual void SetStandardVertexShaderConstants( float fOverbright )
	{
	}
	
	// Level of anisotropic filtering
	virtual void SetAnisotropicLevel( int nAnisotropyLevel );

	bool SupportsHDR() const
	{
		return false;
	}
	HDRType_t GetHDRType() const
	{
		return HDR_TYPE_NONE;
	}
	HDRType_t GetHardwareHDRType() const
	{
		return HDR_TYPE_NONE;
	}
	virtual bool NeedsATICentroidHack() const
	{
		return false;
	}
	virtual bool SupportsColorOnSecondStream() const
	{
		return false;
	}
	virtual bool SupportsStaticPlusDynamicLighting() const
	{
		return false;
	}
	virtual bool SupportsStreamOffset() const
	{
		return false;
	}
	void SetDefaultDynamicState()
	{
	}
	virtual void CommitPixelShaderLighting( int pshReg )
	{
	}

	ShaderAPIOcclusionQuery_t CreateOcclusionQueryObject( void )
	{
		return INVALID_SHADERAPI_OCCLUSION_QUERY_HANDLE;
	}

	void DestroyOcclusionQueryObject( ShaderAPIOcclusionQuery_t handle )
	{
	}

	void BeginOcclusionQueryDrawing( ShaderAPIOcclusionQuery_t handle )
	{
	}

	void EndOcclusionQueryDrawing( ShaderAPIOcclusionQuery_t handle )
	{
	}

	int OcclusionQuery_GetNumPixelsRendered( ShaderAPIOcclusionQuery_t handle, bool bFlush )
	{
		return 0;
	}

	virtual void AcquireThreadOwnership() {}
	virtual void ReleaseThreadOwnership() {}

	virtual bool SupportsBorderColor() const { return false; }
	virtual bool SupportsFetch4() const { return false; }
	virtual bool CanStretchRectFromTextures( void ) const { return false; }
	virtual void EnableBuffer2FramesAhead( bool bEnable ) {}

	virtual void SetPSNearAndFarZ( int pshReg ) { }

	virtual void SetDepthFeatheringPixelShaderConstant( int iConstant, float fDepthBlendScale ) {}

	void SetPixelShaderFogParams( int reg )
	{
	}

	virtual bool InFlashlightMode() const
	{
		return false;
	}

	virtual bool InEditorMode() const
	{
		return false;
	}

	// What fields in the morph do we actually use?
	virtual MorphFormat_t ComputeMorphFormat( int numSnapshots, StateSnapshot_t* pIds ) const
	{
		return 0;
	}

	// Gets the bound morph's vertex format; returns 0 if no morph is bound
	virtual MorphFormat_t GetBoundMorphFormat()
	{
		return 0;
	}

	// Binds a standard texture
	virtual void BindStandardTexture( Sampler_t stage, StandardTextureId_t id )
	{
	}

	virtual void BindStandardVertexTexture( VertexTextureSampler_t stage, StandardTextureId_t id )
	{
	}

	virtual void GetStandardTextureDimensions( int *pWidth, int *pHeight, StandardTextureId_t id )
	{
		*pWidth = *pHeight = 0;
	}


	virtual void SetFlashlightState( const FlashlightState_t &state, const VMatrix &worldToTexture )
	{
	}

	virtual void SetFlashlightStateEx( const FlashlightState_t &state, const VMatrix &worldToTexture, ITexture *pFlashlightDepthTexture )
	{
	}

	virtual const FlashlightState_t &GetFlashlightState( VMatrix &worldToTexture ) const 
	{
		static FlashlightState_t  blah;
		return blah;
	}

	virtual const FlashlightState_t &GetFlashlightStateEx( VMatrix &worldToTexture, ITexture **pFlashlightDepthTexture ) const 
	{
		static FlashlightState_t  blah;
		return blah;
	}

	virtual void ClearVertexAndPixelShaderRefCounts()
	{
	}

	virtual void PurgeUnusedVertexAndPixelShaders()
	{
	}

	virtual bool IsAAEnabled() const
	{
		return false;
	}

	virtual int GetVertexTextureCount() const
	{
		return 0;
	}

	virtual int GetMaxVertexTextureDimension() const
	{
		return 0;
	}

	virtual int  MaxTextureDepth() const
	{
		return 0;
	}

	// Binds a vertex texture to a particular texture stage in the vertex pipe
	virtual void BindVertexTexture( VertexTextureSampler_t nSampler, ShaderAPITextureHandle_t hTexture )
	{
	}

	// Sets morph target factors
	virtual void SetFlexWeights( int nFirstWeight, int nCount, const MorphWeight_t* pWeights )
	{
	}

	// NOTE: Stuff after this is added after shipping HL2.
	ITexture *GetRenderTargetEx( int nRenderTargetID )
	{
		return NULL;
	}

	void SetToneMappingScaleLinear( const Vector &scale )
	{
	}

	const Vector &GetToneMappingScaleLinear( void ) const
	{
		static Vector dummy;
		return dummy;
	}

	virtual float GetLightMapScaleFactor( void ) const
	{
		return 1.0;
	}


	// For dealing with device lost in cases where SwapBuffers isn't called all the time (Hammer)
	virtual void HandleDeviceLost()
	{
	}

	virtual void EnableLinearColorSpaceFrameBuffer( bool bEnable )
	{
	}

	// Lets the shader know about the full-screen texture so it can 
	virtual void SetFullScreenTextureHandle( ShaderAPITextureHandle_t h )
	{
	}

	void SetFloatRenderingParameter(int parm_number, float value)
	{
	}

	void SetIntRenderingParameter(int parm_number, int value)
	{
	}
	void SetVectorRenderingParameter(int parm_number, Vector const &value)
	{
	}

	float GetFloatRenderingParameter(int parm_number) const
	{
		return 0;
	}

	int GetIntRenderingParameter(int parm_number) const
	{
		return 0;
	}

	Vector GetVectorRenderingParameter(int parm_number) const
	{
		return Vector(0,0,0);
	}

	// Methods related to stencil
	void SetStencilEnable(bool onoff)
	{
	}

	void SetStencilFailOperation(StencilOperation_t op)
	{
	}

	void SetStencilZFailOperation(StencilOperation_t op)
	{
	}

	void SetStencilPassOperation(StencilOperation_t op)
	{
	}

	void SetStencilCompareFunction(StencilComparisonFunction_t cmpfn)
	{
	}

	void SetStencilReferenceValue(int ref)
	{
	}

	void SetStencilTestMask(uint32 msk)
	{
	}

	void SetStencilWriteMask(uint32 msk)
	{
	}

	void ClearStencilBufferRectangle( int xmin, int ymin, int xmax, int ymax,int value)
	{
	}

	virtual void GetDXLevelDefaults(uint &max_dxlevel,uint &recommended_dxlevel)
	{
		max_dxlevel=recommended_dxlevel=90;
	}

	virtual void GetMaxToRender( IMesh *pMesh, bool bMaxUntilFlush, int *pMaxVerts, int *pMaxIndices )
	{
		*pMaxVerts = 32768;
		*pMaxIndices = 32768;
	}

	// Returns the max possible vertices + indices to render in a single draw call
	virtual int GetMaxVerticesToRender( IMaterial *pMaterial )
	{
		return 32768;
	}

	virtual int GetMaxIndicesToRender( )
	{
		return 32768;
	}
	virtual int CompareSnapshots( StateSnapshot_t snapshot0, StateSnapshot_t snapshot1 ) { return 0; }

	virtual void DisableAllLocalLights() {}

	virtual bool SupportsMSAAMode( int nMSAAMode ) { return false; }

	virtual bool SupportsCSAAMode( int nNumSamples, int nQualityLevel ) { return false; }

	// Hooks for firing PIX events from outside the Material System...
	virtual void BeginPIXEvent( unsigned long color, const char *szName ) {}
	virtual void EndPIXEvent() {}
	virtual void SetPIXMarker( unsigned long color, const char *szName ) {}

	virtual void ComputeVertexDescription( unsigned char* pBuffer, VertexFormat_t vertexFormat, MeshDesc_t& desc ) const {}

	virtual bool SupportsShadowDepthTextures() { return false; }

	virtual bool SupportsFetch4() { return false; }

	virtual int NeedsShaderSRGBConversion(void) const { return 0; }
	virtual bool UsesSRGBCorrectBlending() const { return false; }

	virtual bool HasFastVertexTextures() const { return false; }

	virtual void SetShadowDepthBiasFactors( float fShadowSlopeScaleDepthBias, float fShadowDepthBias ) {}

	virtual void SetDisallowAccess( bool ) {}
	virtual void EnableShaderShaderMutex( bool ) {}
	virtual void ShaderLock() {}
	virtual void ShaderUnlock() {}

// ------------ New Vertex/Index Buffer interface ----------------------------
	void BindVertexBuffer( int streamID, IVertexBuffer *pVertexBuffer, int nOffsetInBytes, int nFirstVertex, int nVertexCount, VertexFormat_t fmt, int nRepetitions1 )
	{
	}
	void BindIndexBuffer( IIndexBuffer *pIndexBuffer, int nOffsetInBytes )
	{
	}
	void Draw( MaterialPrimitiveType_t primitiveType, int firstIndex, int numIndices )
	{
	}
// ------------ End ----------------------------

	virtual int  GetVertexBufferCompression( void ) const { return 0; };

	virtual bool ShouldWriteDepthToDestAlpha( void ) const { return false; };
	virtual bool SupportsHDRMode( HDRType_t nHDRMode ) const { return false; };
	virtual bool IsDX10Card() const { return false; };

	void PushDeformation( const DeformationBase_t *pDeformation )
	{
	}

	virtual void PopDeformation( )
	{
	}

	int GetNumActiveDeformations( ) const
	{
		return 0;
	}

	// for shaders to set vertex shader constants. returns a packed state which can be used to set the dynamic combo
	int GetPackedDeformationInformation( int nMaskOfUnderstoodDeformations,
										 float *pConstantValuesOut,
										 int nBufferSize,
										 int nMaximumDeformations,
										 int *pNumDefsOut ) const
	{
		*pNumDefsOut = 0;
		return 0;
	}

	void SetStandardTextureHandle(StandardTextureId_t,ShaderAPITextureHandle_t)
	{
	}

	virtual void ExecuteCommandBuffer( uint8 *pData )
	{
	}
	virtual bool GetHDREnabled( void ) const { return true; }
	virtual void SetHDREnabled( bool bEnable ) {}

	virtual void CopyRenderTargetToScratchTexture( ShaderAPITextureHandle_t srcRt, ShaderAPITextureHandle_t dstTex, Rect_t *pSrcRect = NULL, Rect_t *pDstRect = NULL ) 
	{
	}

	// Allows locking and unlocking of very specific surface types.
	virtual void LockRect( void** pOutBits, int* pOutPitch, ShaderAPITextureHandle_t texHandle, int mipmap, int x, int y, int w, int h, bool bWrite, bool bRead ) 
	{
	}

	virtual void UnlockRect( ShaderAPITextureHandle_t texHandle, int mipmap )
	{
	}

	virtual void TexLodClamp( int finest ) {}

	virtual void TexLodBias( float bias ) {}

	virtual void CopyTextureToTexture( ShaderAPITextureHandle_t srcTex, ShaderAPITextureHandle_t dstTex ) {}
	
	void PrintfVA( char *fmt, va_list vargs ) {}
	void Printf( const char *fmt, ... ) {}
	float Knob( char *knobname, float *setvalue = NULL ) { return 0.0f; };

private:
	enum
	{
		TRANSLUCENT = 0x1,
		ALPHATESTED = 0x2,
		VERTEX_AND_PIXEL_SHADERS = 0x4,
		DEPTHWRITE = 0x8,
	};

	CEmptyMesh m_Mesh;

	void EnableAlphaToCoverage() {} ;
	void DisableAlphaToCoverage() {} ;

	ImageFormat GetShadowDepthTextureFormat() { return IMAGE_FORMAT_UNKNOWN; };
	ImageFormat GetNullTextureFormat() { return IMAGE_FORMAT_UNKNOWN; };
};


//-----------------------------------------------------------------------------
// Class Factory
//-----------------------------------------------------------------------------

static CShaderAPIEmpty g_ShaderAPIEmpty;
static CShaderShadowEmpty g_ShaderShadow;

// FIXME: Remove; it's for backward compat with the materialsystem only for now
EXPOSE_SINGLE_INTERFACE_GLOBALVAR( CShaderAPIEmpty, IShaderAPI, 
									SHADERAPI_INTERFACE_VERSION, g_ShaderAPIEmpty )

EXPOSE_SINGLE_INTERFACE_GLOBALVAR( CShaderShadowEmpty, IShaderShadow, 
								SHADERSHADOW_INTERFACE_VERSION, g_ShaderShadow )

EXPOSE_SINGLE_INTERFACE_GLOBALVAR( CShaderAPIEmpty, IMaterialSystemHardwareConfig, 
				MATERIALSYSTEM_HARDWARECONFIG_INTERFACE_VERSION, g_ShaderAPIEmpty )

EXPOSE_SINGLE_INTERFACE_GLOBALVAR( CShaderAPIEmpty, IDebugTextureInfo, 
				DEBUG_TEXTURE_INFO_VERSION, g_ShaderAPIEmpty )


//-----------------------------------------------------------------------------
// The main GL Shader util interface
//-----------------------------------------------------------------------------
IShaderUtil* g_pShaderUtil;


//-----------------------------------------------------------------------------
// Factory to return from SetMode
//-----------------------------------------------------------------------------
static void* ShaderInterfaceFactory( const char *pInterfaceName, int *pReturnCode )
{
	if ( pReturnCode )
	{
		*pReturnCode = IFACE_OK;
	}
	if ( !Q_stricmp( pInterfaceName, SHADER_DEVICE_INTERFACE_VERSION ) )
		return static_cast< IShaderDevice* >( &s_ShaderDeviceEmpty );
	if ( !Q_stricmp( pInterfaceName, SHADERAPI_INTERFACE_VERSION ) )
		return static_cast< IShaderAPI* >( &g_ShaderAPIEmpty );
	if ( !Q_stricmp( pInterfaceName, SHADERSHADOW_INTERFACE_VERSION ) )
		return static_cast< IShaderShadow* >( &g_ShaderShadow );

	if ( pReturnCode )
	{
		*pReturnCode = IFACE_FAILED;
	}
	return NULL;
}


//-----------------------------------------------------------------------------
//
// CShaderDeviceMgrEmpty
//
//-----------------------------------------------------------------------------
bool CShaderDeviceMgrEmpty::Connect( CreateInterfaceFn factory )
{
	// So others can access it
	g_pShaderUtil = (IShaderUtil*)factory( SHADER_UTIL_INTERFACE_VERSION, NULL );

	return true;
}

void CShaderDeviceMgrEmpty::Disconnect()
{
	g_pShaderUtil = NULL;
}

void *CShaderDeviceMgrEmpty::QueryInterface( const char *pInterfaceName )
{
	if ( !Q_stricmp( pInterfaceName, SHADER_DEVICE_MGR_INTERFACE_VERSION ) )
		return static_cast< IShaderDeviceMgr* >( this );
	if ( !Q_stricmp( pInterfaceName, MATERIALSYSTEM_HARDWARECONFIG_INTERFACE_VERSION ) )
		return static_cast< IMaterialSystemHardwareConfig* >( &g_ShaderAPIEmpty );
	return NULL;
}

InitReturnVal_t CShaderDeviceMgrEmpty::Init()
{
	return INIT_OK;
}

void CShaderDeviceMgrEmpty::Shutdown()
{

}

// Sets the adapter
bool CShaderDeviceMgrEmpty::SetAdapter( int nAdapter, int nFlags )
{
	return true;
}

// FIXME: Is this a public interface? Might only need to be private to shaderapi
// Note: not actually reached in this build's real init flow - materialsystem
// fetches IShaderDevice straight from the EXPOSE_SINGLE_INTERFACE_GLOBALVAR
// registration instead of through this CreateInterfaceFn indirection.
// VR session creation lives in CShaderDeviceEmpty::Present() instead, since
// that's confirmed-called every frame.
CreateInterfaceFn CShaderDeviceMgrEmpty::SetMode( void *hWnd, int nAdapter, const ShaderDeviceInfo_t& mode )
{
	return ShaderInterfaceFactory;
}

// Gets the number of adapters...
int	 CShaderDeviceMgrEmpty::GetAdapterCount() const
{
	return 0;
}

bool CShaderDeviceMgrEmpty::GetRecommendedConfigurationInfo( int nAdapter, int nDXLevel, KeyValues *pKeyValues ) 
{
	return true;
}

// Returns info about each adapter
void CShaderDeviceMgrEmpty::GetAdapterInfo( int adapter, MaterialAdapterInfo_t& info ) const
{
	memset( &info, 0, sizeof( info ) );
	info.m_nDXSupportLevel = 90;
}

// Returns the number of modes
int	 CShaderDeviceMgrEmpty::GetModeCount( int nAdapter ) const
{
	return 0;
}

// Returns mode information..
void CShaderDeviceMgrEmpty::GetModeInfo( ShaderDisplayMode_t *pInfo, int nAdapter, int nMode ) const
{
}

void CShaderDeviceMgrEmpty::GetCurrentModeInfo( ShaderDisplayMode_t* pInfo, int nAdapter ) const
{
}


//-----------------------------------------------------------------------------
//
// Shader device empty
//
//-----------------------------------------------------------------------------
void CShaderDeviceEmpty::GetWindowSize( int &width, int &height ) const
{
	width = 0;
	height = 0;
}

void CShaderDeviceEmpty::GetBackBufferDimensions( int& width, int& height ) const
{
	width = 1024;
	height = 768;
}

// Use this to spew information about the 3D layer 
void CShaderDeviceEmpty::SpewDriverInfo() const
{
	Warning("Empty shader\n");
}

// Creates/ destroys a child window
bool CShaderDeviceEmpty::AddView( void* hwnd )
{
	return true;
}

void CShaderDeviceEmpty::RemoveView( void* hwnd )
{
}

// Activates a view
void CShaderDeviceEmpty::SetView( void* hwnd )
{
}

void CShaderDeviceEmpty::ReleaseResources()
{
}

void CShaderDeviceEmpty::ReacquireResources()
{
}

// Creates/destroys Mesh
IMesh* CShaderDeviceEmpty::CreateStaticMesh( VertexFormat_t fmt, const char *pTextureBudgetGroup, IMaterial * pMaterial )
{
	return &m_Mesh;
}

void CShaderDeviceEmpty::DestroyStaticMesh( IMesh* mesh )
{
}

// Creates/destroys static vertex + index buffers
IVertexBuffer *CShaderDeviceEmpty::CreateVertexBuffer( ShaderBufferType_t type, VertexFormat_t fmt, int nVertexCount, const char *pTextureBudgetGroup )
{
	return ( type == SHADER_BUFFER_TYPE_STATIC || type == SHADER_BUFFER_TYPE_STATIC_TEMP ) ? &m_Mesh : &m_DynamicMesh;
}

void CShaderDeviceEmpty::DestroyVertexBuffer( IVertexBuffer *pVertexBuffer )
{

}

IIndexBuffer *CShaderDeviceEmpty::CreateIndexBuffer( ShaderBufferType_t bufferType, MaterialIndexFormat_t fmt, int nIndexCount, const char *pTextureBudgetGroup )
{
	switch( bufferType )
	{
	case SHADER_BUFFER_TYPE_STATIC:
	case SHADER_BUFFER_TYPE_STATIC_TEMP:
		return &m_Mesh;
	default:
		Assert( 0 );
	case SHADER_BUFFER_TYPE_DYNAMIC:
	case SHADER_BUFFER_TYPE_DYNAMIC_TEMP:
		return &m_DynamicMesh;
	}
}

void CShaderDeviceEmpty::DestroyIndexBuffer( IIndexBuffer *pIndexBuffer )
{

}

IVertexBuffer *CShaderDeviceEmpty::GetDynamicVertexBuffer( int streamID, VertexFormat_t vertexFormat, bool bBuffered )
{
	return &m_DynamicMesh;
}

IIndexBuffer *CShaderDeviceEmpty::GetDynamicIndexBuffer( MaterialIndexFormat_t fmt, bool bBuffered )
{
	return &m_Mesh;
}



//-----------------------------------------------------------------------------
//
// The empty mesh...
//
//-----------------------------------------------------------------------------
CEmptyMesh::CEmptyMesh( bool bIsDynamic ) : m_bIsDynamic( bIsDynamic )
{
	m_pVertexMemory = new unsigned char[VERTEX_BUFFER_SIZE];
#if defined( ANDROID )
	m_pIndexMemory = new unsigned short[INDEX_BUFFER_COUNT];
	memset( m_DummyAttrib, 0, sizeof( m_DummyAttrib ) );
	m_bGpuBuffersReady = false;
	m_hVB = VK_NULL_HANDLE;
	m_hVBMem = VK_NULL_HANDLE;
	m_pMappedVB = NULL;
	m_hIB = VK_NULL_HANDLE;
	m_hIBMem = VK_NULL_HANDLE;
	m_pMappedIB = NULL;
	m_nPendingVerts = 0;
	m_nPendingIndices = 0;
	m_nDrawIndexCount = 0;
#endif
}

CEmptyMesh::~CEmptyMesh()
{
	delete[] m_pVertexMemory;
#if defined( ANDROID )
	delete[] m_pIndexMemory;
	if ( m_bGpuBuffersReady )
	{
		vkDestroyBuffer( VRXR_GetDevice(), m_hVB, NULL );
		vkFreeMemory( VRXR_GetDevice(), m_hVBMem, NULL );
		vkDestroyBuffer( VRXR_GetDevice(), m_hIB, NULL );
		vkFreeMemory( VRXR_GetDevice(), m_hIBMem, NULL );
	}
#endif
}

#if defined( ANDROID )
// Allocates and persistently maps this mesh's GPU vertex/index buffers on
// first use (once the Vulkan device actually exists - Lock()/UnlockMesh()
// get called well before that, e.g. for early UI/font setup). Deliberately
// NOT per-Lock/Unlock: this codebase re-locks and re-uploads these two
// shared mesh objects constantly (every world surface during level load,
// every dynamic/UI draw every frame), and a fresh vkAllocateMemory +
// vkFreeMemory per cycle turned out to be both slow enough to look like a
// hang and, when destruction was ever deferred instead, enough to exhaust
// the device's live allocation budget (VK_ERROR_OUT_OF_DEVICE_MEMORY). One
// allocation per mesh object, kept for the process lifetime, sidesteps
// both problems.
bool CEmptyMesh::EnsureGpuBuffers()
{
	if ( m_bGpuBuffersReady )
		return true;

	if ( VRXR_GetDevice() == VK_NULL_HANDLE )
		return false;

	HL2VR_DIAG( "EnsureGpuBuffers: creating (mesh=%p dynamic=%d)", (void*)this, (int)m_bIsDynamic );

	VkMemoryPropertyFlags hostProps = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	if ( !VRXR_CreateBuffer( VERTEX_BUFFER_SIZE, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, hostProps, &m_hVB, &m_hVBMem ) )
	{
		HL2VR_DIAG( "EnsureGpuBuffers: VB create failed" );
		return false;
	}
	if ( !VRXR_CreateBuffer( INDEX_BUFFER_COUNT * sizeof( unsigned short ), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, hostProps, &m_hIB, &m_hIBMem ) )
	{
		HL2VR_DIAG( "EnsureGpuBuffers: IB create failed" );
		vkDestroyBuffer( VRXR_GetDevice(), m_hVB, NULL );
		vkFreeMemory( VRXR_GetDevice(), m_hVBMem, NULL );
		m_hVB = VK_NULL_HANDLE;
		m_hVBMem = VK_NULL_HANDLE;
		return false;
	}

	vkMapMemory( VRXR_GetDevice(), m_hVBMem, 0, VERTEX_BUFFER_SIZE, 0, &m_pMappedVB );
	vkMapMemory( VRXR_GetDevice(), m_hIBMem, 0, INDEX_BUFFER_COUNT * sizeof( unsigned short ), 0, &m_pMappedIB );

	HL2VR_DIAG( "EnsureGpuBuffers: ready (mesh=%p)", (void*)this );
	m_bGpuBuffersReady = true;
	return true;
}

void CEmptyMesh::UploadToGpu( int numVerts, int numIndices )
{
	m_nDrawIndexCount = 0;

	if ( numVerts <= 0 || numIndices <= 0 )
		return;

	if ( (size_t)numVerts * 20 > VERTEX_BUFFER_SIZE || (size_t)numIndices > INDEX_BUFFER_COUNT )
		return;

	// Device (and therefore these buffers) may not exist yet this early -
	// that's fine, this lock/unlock's data just won't be drawable until a
	// later re-lock happens once VRXR_Init() has run.
	if ( !EnsureGpuBuffers() )
		return;

	memcpy( m_pMappedVB, m_pVertexMemory, (size_t)numVerts * 20 );
	memcpy( m_pMappedIB, m_pIndexMemory, (size_t)numIndices * sizeof( unsigned short ) );

	m_nDrawIndexCount = numIndices;
}
#endif

bool CEmptyMesh::Lock( int nMaxIndexCount, bool bAppend, IndexDesc_t& desc )
{
#if defined( ANDROID )
	if ( nMaxIndexCount > (int)INDEX_BUFFER_COUNT )
		nMaxIndexCount = INDEX_BUFFER_COUNT;
	desc.m_pIndices = m_pIndexMemory;
	desc.m_nIndexSize = 1;
	desc.m_nFirstIndex = 0;
	desc.m_nOffset = 0;
	m_nPendingIndices = nMaxIndexCount;
	return true;
#else
	static int s_BogusIndex;
	desc.m_pIndices = (unsigned short*)&s_BogusIndex;
	desc.m_nIndexSize = 0;
	desc.m_nFirstIndex = 0;
	desc.m_nOffset = 0;
	return true;
#endif
}

void CEmptyMesh::Unlock( int nWrittenIndexCount, IndexDesc_t& desc )
{
}

void CEmptyMesh::ModifyBegin( bool bReadOnly, int nFirstIndex, int nIndexCount, IndexDesc_t& desc )
{
	Lock( nIndexCount, false, desc );
}

void CEmptyMesh::ModifyEnd( IndexDesc_t& desc )
{
}

void CEmptyMesh::Spew( int nIndexCount, const IndexDesc_t & desc )
{
}

void CEmptyMesh::ValidateData( int nIndexCount, const IndexDesc_t &desc )
{
}

bool CEmptyMesh::Lock( int nVertexCount, bool bAppend, VertexDesc_t &desc )
{
#if defined( ANDROID )
	// Fixed layout matching basic.vert / the pipeline's vertex input state in
	// vr_xr_vulkan.cpp: interleaved [ position(3f)@0, texcoord0(2f)@12 ],
	// stride 20 bytes. Every other attribute the engine might write is
	// routed to a small scratch buffer (m_DummyAttrib, stride 0) - harmless
	// since ComputeVertexFormat()/ComputeVertexUsage() only ever report
	// VERTEX_POSITION|VERTEX_TEXCOORD_SIZE(0,2), so CMeshBuilder shouldn't be
	// writing those fields anyway.
	const int stride = 20;
	if ( (size_t)nVertexCount * stride > VERTEX_BUFFER_SIZE )
		nVertexCount = VERTEX_BUFFER_SIZE / stride;

	desc.m_pPosition = (float*)m_pVertexMemory;
	desc.m_VertexSize_Position = stride;

	desc.m_pTexCoord[0] = (float*)( m_pVertexMemory + 12 );
	desc.m_VertexSize_TexCoord[0] = stride;
	for ( int i = 1; i < VERTEX_MAX_TEXTURE_COORDINATES; ++i )
	{
		desc.m_pTexCoord[i] = (float*)m_DummyAttrib;
		desc.m_VertexSize_TexCoord[i] = 0;
	}

	desc.m_pNormal = (float*)m_DummyAttrib;
	desc.m_pColor = (unsigned char*)m_DummyAttrib;
	desc.m_pBoneWeight = (float*)m_DummyAttrib;
	desc.m_pBoneMatrixIndex = (unsigned char*)m_DummyAttrib;
	desc.m_pTangentS = (float*)m_DummyAttrib;
	desc.m_pTangentT = (float*)m_DummyAttrib;
	desc.m_pUserData = (float*)m_DummyAttrib;
	desc.m_NumBoneWeights = 0;

	desc.m_VertexSize_BoneWeight = 0;
	desc.m_VertexSize_BoneMatrixIndex = 0;
	desc.m_VertexSize_Normal = 0;
	desc.m_VertexSize_Color = 0;
	desc.m_VertexSize_TangentS = 0;
	desc.m_VertexSize_TangentT = 0;
	desc.m_VertexSize_UserData = 0;
	desc.m_ActualVertexSize = stride;

	desc.m_nFirstVertex = 0;
	desc.m_nOffset = 0;
	m_nPendingVerts = nVertexCount;
	return true;
#else
	// Who cares about the data?
	desc.m_pPosition = (float*)m_pVertexMemory;
	desc.m_pNormal = (float*)m_pVertexMemory;
	desc.m_pColor = m_pVertexMemory;

	int i;
	for ( i = 0; i < VERTEX_MAX_TEXTURE_COORDINATES; ++i)
	{
		desc.m_pTexCoord[i] = (float*)m_pVertexMemory;
	}

	desc.m_pBoneWeight = (float*)m_pVertexMemory;
	desc.m_pBoneMatrixIndex = (unsigned char*)m_pVertexMemory;
	desc.m_pTangentS = (float*)m_pVertexMemory;
	desc.m_pTangentT = (float*)m_pVertexMemory;
	desc.m_pUserData = (float*)m_pVertexMemory;
	desc.m_NumBoneWeights = 2;

	desc.m_VertexSize_Position = 0;
	desc.m_VertexSize_BoneWeight = 0;
	desc.m_VertexSize_BoneMatrixIndex = 0;
	desc.m_VertexSize_Normal = 0;
	desc.m_VertexSize_Color = 0;
	for( i=0; i < VERTEX_MAX_TEXTURE_COORDINATES; i++ )
	{
		desc.m_VertexSize_TexCoord[i] = 0;
	}
	desc.m_VertexSize_TangentS = 0;
	desc.m_VertexSize_TangentT = 0;
	desc.m_VertexSize_UserData = 0;
	desc.m_ActualVertexSize = 0;	// Size of the vertices.. Some of the m_VertexSize_ elements above

	desc.m_nFirstVertex = 0;
	desc.m_nOffset = 0;
	return true;
#endif
}

void CEmptyMesh::Unlock( int nVertexCount, VertexDesc_t &desc )
{
}

void CEmptyMesh::Spew( int nVertexCount, const VertexDesc_t &desc ) 
{
}

void CEmptyMesh::ValidateData( int nVertexCount, const VertexDesc_t & desc )
{
}

void CEmptyMesh::LockMesh( int numVerts, int numIndices, MeshDesc_t& desc )
{
	Lock( numVerts, false, *static_cast<VertexDesc_t*>( &desc ) );
	Lock( numIndices, false, *static_cast<IndexDesc_t*>( &desc ) );
}

void CEmptyMesh::UnlockMesh( int numVerts, int numIndices, MeshDesc_t& desc )
{
#if defined( ANDROID )
	UploadToGpu( numVerts, numIndices );
#endif
}

void CEmptyMesh::ModifyBeginEx( bool bReadOnly, int firstVertex, int numVerts, int firstIndex, int numIndices, MeshDesc_t& desc )
{
	Lock( numVerts, false, *static_cast<VertexDesc_t*>( &desc ) );
	Lock( numIndices, false, *static_cast<IndexDesc_t*>( &desc ) );
}

void CEmptyMesh::ModifyBegin( int firstVertex, int numVerts, int firstIndex, int numIndices, MeshDesc_t& desc )
{
	ModifyBeginEx( false, firstVertex, numVerts, firstIndex, numIndices, desc );
}

void CEmptyMesh::ModifyEnd( MeshDesc_t& desc )
{
}

// returns the # of vertices (static meshes only)
int CEmptyMesh::VertexCount() const
{
#if defined( ANDROID )
	return m_nPendingVerts;
#else
	return 0;
#endif
}

// Sets the primitive type
void CEmptyMesh::SetPrimitiveType( MaterialPrimitiveType_t type )
{
}

// Draws the entire mesh
void CEmptyMesh::Draw( int firstIndex, int numIndices )
{
#if defined( ANDROID )
	if ( m_hVB == VK_NULL_HANDLE || m_hIB == VK_NULL_HANDLE || m_nDrawIndexCount <= 0 )
		return;

	int first = ( firstIndex > 0 ) ? firstIndex : 0;
	int count = ( numIndices > 0 ) ? numIndices : ( m_nDrawIndexCount - first );
	if ( first + count > m_nDrawIndexCount )
		count = m_nDrawIndexCount - first;
	if ( count <= 0 )
		return;

	float mvp[16];
	ComputeMvp( mvp );

	VkImageView textureView = VK_NULL_HANDLE;
	if ( IsValidTextureHandle( g_BoundTexture0 ) )
	{
		VulkanTexture &tex = GetTexture( g_BoundTexture0 );
		if ( tex.hasData && tex.view != VK_NULL_HANDLE )
			textureView = tex.view;
	}
	if ( textureView == VK_NULL_HANDLE )
		textureView = EnsureWhiteTexture();
	if ( textureView == VK_NULL_HANDLE )
		return;

	VRXR_DrawIndexed( m_hVB, 0, m_hIB, (VkDeviceSize)first * sizeof( unsigned short ),
		(uint32_t)count, mvp, textureView, VRXR_GetDefaultSampler() );
#endif
}

void CEmptyMesh::Draw(CPrimList *pPrims, int nPrims)
{
#if defined( ANDROID )
	for ( int i = 0; i < nPrims; ++i )
		Draw( pPrims[i].m_FirstIndex, pPrims[i].m_NumIndices );
#endif
}

// Copy verts and/or indices to a mesh builder. This only works for temp meshes!
void CEmptyMesh::CopyToMeshBuilder( 
	int iStartVert,		// Which vertices to copy.
	int nVerts, 
	int iStartIndex,	// Which indices to copy.
	int nIndices, 
	int indexOffset,	// This is added to each index.
	CMeshBuilder &builder )
{
}

// Spews the mesh data
void CEmptyMesh::Spew( int numVerts, int numIndices, const MeshDesc_t & desc )
{
}

void CEmptyMesh::ValidateData( int numVerts, int numIndices, const MeshDesc_t & desc )
{
}

// gets the associated material
IMaterial* CEmptyMesh::GetMaterial()
{
	// umm. this don't work none
	Assert(0);
	return 0;
}

//-----------------------------------------------------------------------------
// The shader shadow interface
//-----------------------------------------------------------------------------
CShaderShadowEmpty::CShaderShadowEmpty()
{
	m_IsTranslucent = false;
	m_IsAlphaTested = false;
	m_bIsDepthWriteEnabled = true;
	m_bUsesVertexAndPixelShaders = false;
}

CShaderShadowEmpty::~CShaderShadowEmpty()
{
}

// Sets the default *shadow* state
void CShaderShadowEmpty::SetDefaultState()
{
	m_IsTranslucent = false;
	m_IsAlphaTested = false;
	m_bIsDepthWriteEnabled = true;
	m_bUsesVertexAndPixelShaders = false;
}

// Methods related to depth buffering
void CShaderShadowEmpty::DepthFunc( ShaderDepthFunc_t depthFunc )
{
}

void CShaderShadowEmpty::EnableDepthWrites( bool bEnable )
{
	m_bIsDepthWriteEnabled = bEnable;
}

void CShaderShadowEmpty::EnableDepthTest( bool bEnable )
{
}

void CShaderShadowEmpty::EnablePolyOffset( PolygonOffsetMode_t nOffsetMode )
{
}

// Suppresses/activates color writing 
void CShaderShadowEmpty::EnableColorWrites( bool bEnable )
{
}

// Suppresses/activates alpha writing 
void CShaderShadowEmpty::EnableAlphaWrites( bool bEnable )
{
}

// Methods related to alpha blending
void CShaderShadowEmpty::EnableBlending( bool bEnable )
{
	m_IsTranslucent = bEnable;
}

void CShaderShadowEmpty::BlendFunc( ShaderBlendFactor_t srcFactor, ShaderBlendFactor_t dstFactor )
{
}

// A simpler method of dealing with alpha modulation
void CShaderShadowEmpty::EnableAlphaPipe( bool bEnable )
{
}

void CShaderShadowEmpty::EnableConstantAlpha( bool bEnable )
{
}

void CShaderShadowEmpty::EnableVertexAlpha( bool bEnable )
{
}

void CShaderShadowEmpty::EnableTextureAlpha( TextureStage_t stage, bool bEnable )
{
}


// Alpha testing
void CShaderShadowEmpty::EnableAlphaTest( bool bEnable )
{
	m_IsAlphaTested = bEnable;
}

void CShaderShadowEmpty::AlphaFunc( ShaderAlphaFunc_t alphaFunc, float alphaRef /* [0-1] */ )
{
}


// Wireframe/filled polygons
void CShaderShadowEmpty::PolyMode( ShaderPolyModeFace_t face, ShaderPolyMode_t polyMode )
{
}


// Back face culling
void CShaderShadowEmpty::EnableCulling( bool bEnable )
{
}


// Alpha to coverage
void CShaderShadowEmpty::EnableAlphaToCoverage( bool bEnable )
{
}


// constant color + transparency
void CShaderShadowEmpty::EnableConstantColor( bool bEnable )
{
}

// Indicates the vertex format for use with a vertex shader
// The flags to pass in here come from the VertexFormatFlags_t enum
// If pTexCoordDimensions is *not* specified, we assume all coordinates
// are 2-dimensional
void CShaderShadowEmpty::VertexShaderVertexFormat( unsigned int nFlags, 
												   int nTexCoordCount,
												   int* pTexCoordDimensions,
												   int nUserDataSize )
{
}

// Indicates we're going to light the model
void CShaderShadowEmpty::EnableLighting( bool bEnable )
{
}

void CShaderShadowEmpty::EnableSpecular( bool bEnable )
{
}

// Activate/deactivate skinning
void CShaderShadowEmpty::EnableVertexBlend( bool bEnable )
{
}

// per texture unit stuff
void CShaderShadowEmpty::OverbrightValue( TextureStage_t stage, float value )
{
}

void CShaderShadowEmpty::EnableTexture( Sampler_t stage, bool bEnable )
{
}

void CShaderShadowEmpty::EnableCustomPixelPipe( bool bEnable )
{
}

void CShaderShadowEmpty::CustomTextureStages( int stageCount )
{
}

void CShaderShadowEmpty::CustomTextureOperation( TextureStage_t stage, ShaderTexChannel_t channel, 
	ShaderTexOp_t op, ShaderTexArg_t arg1, ShaderTexArg_t arg2 )
{
}

void CShaderShadowEmpty::EnableTexGen( TextureStage_t stage, bool bEnable )
{
}

void CShaderShadowEmpty::TexGen( TextureStage_t stage, ShaderTexGenParam_t param )
{
}

// Sets the vertex and pixel shaders
void CShaderShadowEmpty::SetVertexShader( const char *pShaderName, int vshIndex )
{
	m_bUsesVertexAndPixelShaders = ( pShaderName != NULL );
}

void CShaderShadowEmpty::EnableBlendingSeparateAlpha( bool bEnable )
{
}
void CShaderShadowEmpty::SetPixelShader( const char *pShaderName, int pshIndex )
{
	m_bUsesVertexAndPixelShaders = ( pShaderName != NULL );
}

void CShaderShadowEmpty::BlendFuncSeparateAlpha( ShaderBlendFactor_t srcFactor, ShaderBlendFactor_t dstFactor )
{
}
// indicates what per-vertex data we're providing
void CShaderShadowEmpty::DrawFlags( unsigned int drawFlags )
{
}



//-----------------------------------------------------------------------------
//
// Shader API Empty
//
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// Constructor, destructor
//-----------------------------------------------------------------------------

CShaderAPIEmpty::CShaderAPIEmpty()  : m_Mesh( false )
{
}

CShaderAPIEmpty::~CShaderAPIEmpty()
{
}


bool CShaderAPIEmpty::DoRenderTargetsNeedSeparateDepthBuffer() const
{
	return false;
}

// Can we download textures?
bool CShaderAPIEmpty::CanDownloadTextures() const
{
	return false;
}

// Used to clear the transition table when we know it's become invalid.
void CShaderAPIEmpty::ClearSnapshots()
{
}

// Members of IMaterialSystemHardwareConfig
bool CShaderAPIEmpty::HasDestAlphaBuffer() const
{
	return false;
}

bool CShaderAPIEmpty::HasStencilBuffer() const
{
	return false;
}

int CShaderAPIEmpty::MaxViewports() const
{
	return 1;
}

int CShaderAPIEmpty::GetShadowFilterMode() const
{
	return 0;
}

int CShaderAPIEmpty::StencilBufferBits() const
{
	return 0;
}

int	 CShaderAPIEmpty::GetFrameBufferColorDepth() const
{
	return 0;
}

int  CShaderAPIEmpty::GetSamplerCount() const
{
	if ((ShaderUtil()->GetConfig().dxSupportLevel > 0) &&
	    (ShaderUtil()->GetConfig().dxSupportLevel < 60))
		return 1;
	if (( ShaderUtil()->GetConfig().dxSupportLevel >= 60 ) && ( ShaderUtil()->GetConfig().dxSupportLevel < 80 ))
		return 2;
	return 4;
}

bool CShaderAPIEmpty::HasSetDeviceGammaRamp() const
{
	return false;
}

bool CShaderAPIEmpty::SupportsCompressedTextures() const
{
	return false;
}

VertexCompressionType_t CShaderAPIEmpty::SupportsCompressedVertices() const
{
	return VERTEX_COMPRESSION_NONE;
}

bool CShaderAPIEmpty::SupportsVertexAndPixelShaders() const
{
	if ((ShaderUtil()->GetConfig().dxSupportLevel > 0) &&
	    (ShaderUtil()->GetConfig().dxSupportLevel < 80))
		return false;

	return true;
}

bool CShaderAPIEmpty::SupportsPixelShaders_1_4() const
{
	if ((ShaderUtil()->GetConfig().dxSupportLevel > 0) &&
	    (ShaderUtil()->GetConfig().dxSupportLevel < 81))
		return false;

	return true;
}

bool CShaderAPIEmpty::SupportsPixelShaders_2_0() const
{
	if ((ShaderUtil()->GetConfig().dxSupportLevel > 0) &&
	    (ShaderUtil()->GetConfig().dxSupportLevel < 90))
		return false;

	return true;
}

bool CShaderAPIEmpty::SupportsPixelShaders_2_b() const
{
	if ((ShaderUtil()->GetConfig().dxSupportLevel > 0) &&
	    (ShaderUtil()->GetConfig().dxSupportLevel < 90))
		return false;

	return true;
}

bool CShaderAPIEmpty::ActuallySupportsPixelShaders_2_b() const
{
	return true;
}

bool CShaderAPIEmpty::SupportsShaderModel_3_0() const
{
	if ((ShaderUtil()->GetConfig().dxSupportLevel > 0) &&
		(ShaderUtil()->GetConfig().dxSupportLevel < 95))
		return false;

	return true;
}

bool CShaderAPIEmpty::SupportsStaticControlFlow() const
{
	if ( IsOpenGL() )
		return false;

	return SupportsVertexShaders_2_0();
}

bool CShaderAPIEmpty::SupportsVertexShaders_2_0() const
{
	if ((ShaderUtil()->GetConfig().dxSupportLevel > 0) &&
	    (ShaderUtil()->GetConfig().dxSupportLevel < 90))
		return false;

	return true;
}

int  CShaderAPIEmpty::MaximumAnisotropicLevel() const
{
	return 0;
}

void CShaderAPIEmpty::SetAnisotropicLevel( int nAnisotropyLevel )
{
}

int  CShaderAPIEmpty::MaxTextureWidth() const
{
	// Should be big enough to cover all cases
	return 16384;
}

int  CShaderAPIEmpty::MaxTextureHeight() const
{
	// Should be big enough to cover all cases
	return 16384;
}

int  CShaderAPIEmpty::MaxTextureAspectRatio() const
{
	// Should be big enough to cover all cases
	return 16384;
}


int	 CShaderAPIEmpty::TextureMemorySize() const
{
	// fake it
	return 64 * 1024 * 1024;
}

int  CShaderAPIEmpty::GetDXSupportLevel() const 
{ 
	return 90; 
}

bool CShaderAPIEmpty::SupportsOverbright() const
{
	return false;
}

bool CShaderAPIEmpty::SupportsCubeMaps() const
{
	if ((ShaderUtil()->GetConfig().dxSupportLevel > 0) &&
	    (ShaderUtil()->GetConfig().dxSupportLevel < 70))
		return false;

	return true;
}

bool CShaderAPIEmpty::SupportsNonPow2Textures() const
{
	return true;
}

bool CShaderAPIEmpty::SupportsMipmappedCubemaps() const
{
	if ((ShaderUtil()->GetConfig().dxSupportLevel > 0) &&
	    (ShaderUtil()->GetConfig().dxSupportLevel < 70))
		return false;

	return true;
}

int  CShaderAPIEmpty::GetTextureStageCount() const
{
	return 4;
}

int	 CShaderAPIEmpty::NumVertexShaderConstants() const
{
	return 128;
}

int	 CShaderAPIEmpty::NumBooleanVertexShaderConstants() const
{
	return 0;
}

int	 CShaderAPIEmpty::NumIntegerVertexShaderConstants() const
{
	return 0;
}

int	 CShaderAPIEmpty::NumPixelShaderConstants() const
{
	return 8;
}

int	 CShaderAPIEmpty::MaxNumLights() const
{
	return 4;
}

bool CShaderAPIEmpty::SupportsSpheremapping() const
{
	return false;
}


// This is the max dx support level supported by the card
int	CShaderAPIEmpty::GetMaxDXSupportLevel() const
{
	return 90;
}

bool CShaderAPIEmpty::SupportsHardwareLighting() const
{
	if ((ShaderUtil()->GetConfig().dxSupportLevel > 0) &&
	    (ShaderUtil()->GetConfig().dxSupportLevel < 70))
		return false;

	return true;
}

int	 CShaderAPIEmpty::MaxBlendMatrices() const
{
	if ((ShaderUtil()->GetConfig().dxSupportLevel > 0) &&
	    (ShaderUtil()->GetConfig().dxSupportLevel < 70))
	{
		return 1;
	}

	return 0;
}

int	 CShaderAPIEmpty::MaxBlendMatrixIndices() const
{
	if ((ShaderUtil()->GetConfig().dxSupportLevel > 0) &&
	    (ShaderUtil()->GetConfig().dxSupportLevel < 70))
	{
		return 1;
	}

	return 0;
}

int	 CShaderAPIEmpty::MaxVertexShaderBlendMatrices() const
{
	return 0;
}

int	CShaderAPIEmpty::MaxUserClipPlanes() const
{
	return 0;
}

bool CShaderAPIEmpty::SpecifiesFogColorInLinearSpace() const
{
	return false;
}

bool CShaderAPIEmpty::SupportsSRGB() const
{
	return false;
}

bool CShaderAPIEmpty::FakeSRGBWrite() const
{
	return false;
}

bool CShaderAPIEmpty::CanDoSRGBReadFromRTs() const
{
	return true;
}

bool CShaderAPIEmpty::SupportsGLMixedSizeTargets() const
{
	return false;
}

const char *CShaderAPIEmpty::GetHWSpecificShaderDLLName() const
{
	return 0;
}

// Sets the default *dynamic* state
void CShaderAPIEmpty::SetDefaultState()
{
}


// Returns the snapshot id for the shader state
StateSnapshot_t	 CShaderAPIEmpty::TakeSnapshot( )
{
	StateSnapshot_t id = 0;
	if (g_ShaderShadow.m_IsTranslucent)
		id |= TRANSLUCENT;
	if (g_ShaderShadow.m_IsAlphaTested)
		id |= ALPHATESTED;
	if (g_ShaderShadow.m_bUsesVertexAndPixelShaders)
		id |= VERTEX_AND_PIXEL_SHADERS;
	if (g_ShaderShadow.m_bIsDepthWriteEnabled)
		id |= DEPTHWRITE;
	return id;
}

// Returns true if the state snapshot is transparent
bool CShaderAPIEmpty::IsTranslucent( StateSnapshot_t id ) const
{
	return (id & TRANSLUCENT) != 0; 
}

bool CShaderAPIEmpty::IsAlphaTested( StateSnapshot_t id ) const
{
	return (id & ALPHATESTED) != 0; 
}

bool CShaderAPIEmpty::IsDepthWriteEnabled( StateSnapshot_t id ) const
{
	return (id & DEPTHWRITE) != 0; 
}

bool CShaderAPIEmpty::UsesVertexAndPixelShaders( StateSnapshot_t id ) const
{
	return (id & VERTEX_AND_PIXEL_SHADERS) != 0; 
}

// Gets the vertex format for a set of snapshot ids
VertexFormat_t CShaderAPIEmpty::ComputeVertexFormat( int numSnapshots, StateSnapshot_t* pIds ) const
{
#if defined( ANDROID )
	// Our fixed pipeline only ever writes position + texcoord0 (see
	// CEmptyMesh::Lock/basic.vert) - report that unconditionally so
	// CMeshBuilder actually writes real data instead of treating
	// Position3fv/TexCoord2f as writes to dummy memory.
	return VERTEX_POSITION | VERTEX_TEXCOORD_SIZE( 0, 2 );
#else
	return 0;
#endif
}

// Gets the vertex format for a set of snapshot ids
VertexFormat_t CShaderAPIEmpty::ComputeVertexUsage( int numSnapshots, StateSnapshot_t* pIds ) const
{
#if defined( ANDROID )
	return VERTEX_POSITION | VERTEX_TEXCOORD_SIZE( 0, 2 );
#else
	return 0;
#endif
}

// Uses a state snapshot
void CShaderAPIEmpty::UseSnapshot( StateSnapshot_t snapshot )
{
}

// Sets the color to modulate by
void CShaderAPIEmpty::Color3f( float r, float g, float b )
{
}

void CShaderAPIEmpty::Color3fv( float const* pColor )
{
}

void CShaderAPIEmpty::Color4f( float r, float g, float b, float a )
{
}

void CShaderAPIEmpty::Color4fv( float const* pColor )
{
}

// Faster versions of color
void CShaderAPIEmpty::Color3ub( unsigned char r, unsigned char g, unsigned char b )
{
}

void CShaderAPIEmpty::Color3ubv( unsigned char const* rgb )
{
}

void CShaderAPIEmpty::Color4ub( unsigned char r, unsigned char g, unsigned char b, unsigned char a )
{
}

void CShaderAPIEmpty::Color4ubv( unsigned char const* rgba )
{
}

// The shade mode
void CShaderAPIEmpty::ShadeMode( ShaderShadeMode_t mode )
{
}

// Binds a particular material to render with
void CShaderAPIEmpty::Bind( IMaterial* pMaterial )
{
}

// Cull mode
void CShaderAPIEmpty::CullMode( MaterialCullMode_t cullMode )
{
}

void CShaderAPIEmpty::ForceDepthFuncEquals( bool bEnable )
{
}

// Forces Z buffering on or off
void CShaderAPIEmpty::OverrideDepthEnable( bool bEnable, bool bDepthEnable )
{
}

void CShaderAPIEmpty::OverrideAlphaWriteEnable( bool bOverrideEnable, bool bAlphaWriteEnable )
{
}

void CShaderAPIEmpty::OverrideColorWriteEnable( bool bOverrideEnable, bool bColorWriteEnable )
{
}

//legacy fast clipping linkage
void CShaderAPIEmpty::SetHeightClipZ( float z )
{
}

void CShaderAPIEmpty::SetHeightClipMode( enum MaterialHeightClipMode_t heightClipMode )
{
}

// Sets the lights
void CShaderAPIEmpty::SetLight( int lightNum, const LightDesc_t& desc )
{
}

// Sets lighting origin for the current model
void CShaderAPIEmpty::SetLightingOrigin( Vector vLightingOrigin )
{
}

void CShaderAPIEmpty::SetAmbientLight( float r, float g, float b )
{
}

void CShaderAPIEmpty::SetAmbientLightCube( Vector4D cube[6] )
{
}

// Get lights
int CShaderAPIEmpty::GetMaxLights( void ) const
{
	return 0;
}

const LightDesc_t& CShaderAPIEmpty::GetLight( int lightNum ) const
{
	static LightDesc_t blah;
	return blah;
}

// Render state for the ambient light cube (vertex shaders)
void CShaderAPIEmpty::SetVertexShaderStateAmbientLightCube()
{
}

void CShaderAPIEmpty::SetSkinningMatrices()
{
}

// Lightmap texture binding
void CShaderAPIEmpty::BindLightmap( TextureStage_t stage )
{
}

void CShaderAPIEmpty::BindBumpLightmap( TextureStage_t stage )
{
}

void CShaderAPIEmpty::BindFullbrightLightmap( TextureStage_t stage )
{
}

void CShaderAPIEmpty::BindWhite( TextureStage_t stage )
{
}

void CShaderAPIEmpty::BindBlack( TextureStage_t stage )
{
}

void CShaderAPIEmpty::BindGrey( TextureStage_t stage )
{
}

// Gets the lightmap dimensions
void CShaderAPIEmpty::GetLightmapDimensions( int *w, int *h )
{
	g_pShaderUtil->GetLightmapDimensions( w, h );
}

// Special system flat normal map binding.
void CShaderAPIEmpty::BindFlatNormalMap( TextureStage_t stage )
{
}

void CShaderAPIEmpty::BindNormalizationCubeMap( TextureStage_t stage )
{
}

void CShaderAPIEmpty::BindSignedNormalizationCubeMap( TextureStage_t stage )
{
}

void CShaderAPIEmpty::BindFBTexture( TextureStage_t stage, int textureIndex )
{
}

// Flushes any primitives that are buffered
void CShaderAPIEmpty::FlushBufferedPrimitives()
{
}

// Gets the dynamic mesh; note that you've got to render the mesh
// before calling this function a second time. Clients should *not*
// call DestroyStaticMesh on the mesh returned by this call.
IMesh* CShaderAPIEmpty::GetDynamicMesh( IMaterial* pMaterial, int nHWSkinBoneCount, bool buffered, IMesh* pVertexOverride, IMesh* pIndexOverride )
{
	return &m_Mesh;
}

IMesh* CShaderAPIEmpty::GetDynamicMeshEx( IMaterial* pMaterial, VertexFormat_t fmt, int nHWSkinBoneCount, bool buffered, IMesh* pVertexOverride, IMesh* pIndexOverride )
{
	return &m_Mesh;
}

IMesh* CShaderAPIEmpty::GetFlexMesh()
{
	return &m_Mesh;
}

// Begins a rendering pass that uses a state snapshot
void CShaderAPIEmpty::BeginPass( StateSnapshot_t snapshot  )
{
}

// Renders a single pass of a material
void CShaderAPIEmpty::RenderPass( int nPass, int nPassCount )
{
}

// stuff related to matrix stacks
void CShaderAPIEmpty::MatrixMode( MaterialMatrixMode_t matrixMode )
{
#if defined( ANDROID )
	g_CurMatMode = matrixMode;
#endif
}

void CShaderAPIEmpty::PushMatrix()
{
#if defined( ANDROID )
	EnsureMatStacksInit();
	MatrixStackState &s = g_MatStacks[g_CurMatMode];
	if ( s.depth + 1 < (int)( sizeof( s.stack ) / sizeof( s.stack[0] ) ) )
	{
		memcpy( s.stack[s.depth + 1], s.stack[s.depth], sizeof( Mat4 ) );
		s.depth++;
	}
#endif
}

void CShaderAPIEmpty::PopMatrix()
{
#if defined( ANDROID )
	EnsureMatStacksInit();
	MatrixStackState &s = g_MatStacks[g_CurMatMode];
	if ( s.depth > 0 )
		s.depth--;
#endif
}

void CShaderAPIEmpty::LoadMatrix( float *m )
{
#if defined( ANDROID )
	// The engine's LoadMatrix()/MultMatrix() calls turned out to hand us
	// matrices in the opposite row/column convention from what we assumed
	// (VMatrix-style row-major) - diagnostic logging showed MVP matrices
	// with wildly large, erratic coefficients (translation components
	// landing in the wrong slots during composition), which rendered as
	// scattered degenerate triangles/lines instead of solid geometry.
	// Transposing at this boundary (the only place engine-supplied
	// matrices enter our row-major internal storage) fixes it without
	// touching our own self-consistent Translate/Rotate/Scale/Mat4Mul code.
	Mat4 transposed;
	Mat4Transpose( transposed, m );
	memcpy( CurMat( g_CurMatMode ), transposed, sizeof( Mat4 ) );
#endif
}

void CShaderAPIEmpty::MultMatrix( float *m )
{
#if defined( ANDROID )
	Mat4 transposed;
	Mat4Transpose( transposed, m );
	Mat4 &cur = CurMat( g_CurMatMode );
	Mat4 result;
	Mat4Mul( result, cur, transposed );
	memcpy( cur, result, sizeof( Mat4 ) );
#endif
}

void CShaderAPIEmpty::MultMatrixLocal( float *m )
{
#if defined( ANDROID )
	Mat4 transposed;
	Mat4Transpose( transposed, m );
	Mat4 &cur = CurMat( g_CurMatMode );
	Mat4 result;
	Mat4Mul( result, transposed, cur );
	memcpy( cur, result, sizeof( Mat4 ) );
#endif
}

void CShaderAPIEmpty::GetMatrix( MaterialMatrixMode_t matrixMode, float *dst )
{
#if defined( ANDROID )
	// Symmetric with LoadMatrix()'s transpose - callers that round-trip a
	// matrix out via GetMatrix() and back in via LoadMatrix() should see
	// their own convention preserved.
	Mat4Transpose( dst, CurMat( matrixMode ) );
#endif
}

void CShaderAPIEmpty::LoadIdentity( void )
{
#if defined( ANDROID )
	Mat4Identity( CurMat( g_CurMatMode ) );
#endif
}

void CShaderAPIEmpty::LoadCameraToWorld( void )
{
}

void CShaderAPIEmpty::Ortho( double left, double top, double right, double bottom, double zNear, double zFar )
{
#if defined( ANDROID )
	Mat4 ortho;
	Mat4Identity( ortho );
	ortho[0] = 2.0f / (float)( right - left );
	ortho[5] = 2.0f / (float)( bottom - top ); // Vulkan clip-space Y+ is down, matches top<bottom screen convention directly
	ortho[10] = 1.0f / (float)( zFar - zNear );
	ortho[3] = (float)( -( right + left ) / ( right - left ) );
	ortho[7] = (float)( -( bottom + top ) / ( bottom - top ) );
	ortho[11] = (float)( -zNear / ( zFar - zNear ) );

	Mat4 &cur = CurMat( g_CurMatMode );
	Mat4 result;
	Mat4Mul( result, cur, ortho );
	memcpy( cur, result, sizeof( Mat4 ) );
#endif
}

void CShaderAPIEmpty::PerspectiveX( double fovx, double aspect, double zNear, double zFar )
{
#if defined( ANDROID )
	// Column-vector, row-major, left-handed camera space (X=right, Y=up,
	// Z=forward) mapping to Vulkan's [0,1] clip-space Z - matches how the
	// view matrix the engine hands us via LoadMatrix(MATERIAL_VIEW) is
	// built. Y is negated so the image comes out right-side-up: our
	// swapchain images are rendered with a standard (non Y-flipped) dynamic
	// viewport, so without this the frame would come out upside-down versus
	// Vulkan's Y-down clip space.
	float xScale = 1.0f / tanf( (float)( fovx * ( M_PI / 180.0 ) ) * 0.5f );
	float yScale = -xScale * (float)aspect;
	float zf = (float)zFar;
	float zn = (float)zNear;

	Mat4 proj;
	memset( proj, 0, sizeof( Mat4 ) );
	proj[0] = xScale;
	proj[5] = yScale;
	proj[10] = zf / ( zf - zn );
	proj[11] = -zn * zf / ( zf - zn );
	proj[14] = 1.0f;

	memcpy( CurMat( g_CurMatMode ), proj, sizeof( Mat4 ) );
#endif
}

void CShaderAPIEmpty::PerspectiveOffCenterX( double fovx, double aspect, double zNear, double zFar, double bottom, double top, double left, double right )
{
#if defined( ANDROID )
	// Not yet implemented precisely (off-center/asymmetric frustum, used for
	// some VR/split-screen paths) - fall back to a symmetric projection so
	// there's at least a sane projection matrix rather than a stale/identity
	// one.
	PerspectiveX( fovx, aspect, zNear, zFar );
#endif
}

void CShaderAPIEmpty::PickMatrix( int x, int y, int width, int height )
{
}

void CShaderAPIEmpty::Rotate( float angle, float x, float y, float z )
{
#if defined( ANDROID )
	float rad = angle * ( (float)M_PI / 180.0f );
	float c = cosf( rad );
	float s = sinf( rad );
	float len = sqrtf( x * x + y * y + z * z );
	if ( len < 1e-8f )
		return;
	x /= len; y /= len; z /= len;

	Mat4 rot;
	Mat4Identity( rot );
	rot[0] = c + x * x * ( 1 - c );
	rot[1] = x * y * ( 1 - c ) - z * s;
	rot[2] = x * z * ( 1 - c ) + y * s;
	rot[4] = y * x * ( 1 - c ) + z * s;
	rot[5] = c + y * y * ( 1 - c );
	rot[6] = y * z * ( 1 - c ) - x * s;
	rot[8] = z * x * ( 1 - c ) - y * s;
	rot[9] = z * y * ( 1 - c ) + x * s;
	rot[10] = c + z * z * ( 1 - c );

	Mat4 &cur = CurMat( g_CurMatMode );
	Mat4 result;
	Mat4Mul( result, cur, rot );
	memcpy( cur, result, sizeof( Mat4 ) );
#endif
}

void CShaderAPIEmpty::Translate( float x, float y, float z )
{
#if defined( ANDROID )
	Mat4 t;
	Mat4Identity( t );
	t[3] = x; t[7] = y; t[11] = z;

	Mat4 &cur = CurMat( g_CurMatMode );
	Mat4 result;
	Mat4Mul( result, cur, t );
	memcpy( cur, result, sizeof( Mat4 ) );
#endif
}

void CShaderAPIEmpty::Scale( float x, float y, float z )
{
#if defined( ANDROID )
	Mat4 s;
	Mat4Identity( s );
	s[0] = x; s[5] = y; s[10] = z;

	Mat4 &cur = CurMat( g_CurMatMode );
	Mat4 result;
	Mat4Mul( result, cur, s );
	memcpy( cur, result, sizeof( Mat4 ) );
#endif
}

void CShaderAPIEmpty::ScaleXY( float x, float y )
{
#if defined( ANDROID )
	Scale( x, y, 1.0f );
#endif
}

// Fog methods...
void CShaderAPIEmpty::FogMode( MaterialFogMode_t fogMode )
{
}

void CShaderAPIEmpty::FogStart( float fStart )
{
}

void CShaderAPIEmpty::FogEnd( float fEnd )
{
}

void CShaderAPIEmpty::SetFogZ( float fogZ )
{
}
	
void CShaderAPIEmpty::FogMaxDensity( float flMaxDensity )
{
}

void CShaderAPIEmpty::GetFogDistances( float *fStart, float *fEnd, float *fFogZ )
{
}


void CShaderAPIEmpty::SceneFogColor3ub( unsigned char r, unsigned char g, unsigned char b )
{
}


void CShaderAPIEmpty::SceneFogMode( MaterialFogMode_t fogMode )
{
}

void CShaderAPIEmpty::GetSceneFogColor( unsigned char *rgb )
{
}

MaterialFogMode_t CShaderAPIEmpty::GetSceneFogMode( )
{
	return MATERIAL_FOG_NONE;
}

int CShaderAPIEmpty::GetPixelFogCombo( )
{
	return 0;
}

void CShaderAPIEmpty::FogColor3f( float r, float g, float b )
{
}

void CShaderAPIEmpty::FogColor3fv( float const* rgb )
{
}

void CShaderAPIEmpty::FogColor3ub( unsigned char r, unsigned char g, unsigned char b )
{
}

void CShaderAPIEmpty::FogColor3ubv( unsigned char const* rgb )
{
}

void CShaderAPIEmpty::SetViewports( int nCount, const ShaderViewport_t* pViewports )
{
}

int CShaderAPIEmpty::GetViewports( ShaderViewport_t* pViewports, int nMax ) const
{
	return 1;
}

// Sets the vertex and pixel shaders
void CShaderAPIEmpty::SetVertexShaderIndex( int vshIndex )
{
}

void CShaderAPIEmpty::SetPixelShaderIndex( int pshIndex )
{
}

// Sets the constant registers for vertex and pixel shaders
void CShaderAPIEmpty::SetVertexShaderConstant( int var, float const* pVec, int numConst, bool bForce )
{
}

void CShaderAPIEmpty::SetBooleanVertexShaderConstant( int var, BOOL const* pVec, int numConst, bool bForce )
{
}

void CShaderAPIEmpty::SetIntegerVertexShaderConstant( int var, int const* pVec, int numConst, bool bForce )
{
}

void CShaderAPIEmpty::SetPixelShaderConstant( int var, float const* pVec, int numConst, bool bForce )
{
}

void CShaderAPIEmpty::SetBooleanPixelShaderConstant( int var, BOOL const* pVec, int numBools, bool bForce )
{
}

void CShaderAPIEmpty::SetIntegerPixelShaderConstant( int var, int const* pVec, int numIntVecs, bool bForce )
{
}

void CShaderAPIEmpty::InvalidateDelayedShaderConstants( void )
{
}

float CShaderAPIEmpty::GammaToLinear_HardwareSpecific( float fGamma ) const
{
	return 0.0f;
}

float CShaderAPIEmpty::LinearToGamma_HardwareSpecific( float fLinear ) const
{
	return 0.0f;
}

void CShaderAPIEmpty::SetLinearToGammaConversionTextures( ShaderAPITextureHandle_t hSRGBWriteEnabledTexture, ShaderAPITextureHandle_t hIdentityTexture )
{
}


// Returns the nearest supported format
ImageFormat CShaderAPIEmpty::GetNearestSupportedFormat( ImageFormat fmt, bool bFilteringRequired /* = true */ ) const
{
	return fmt;
}

ImageFormat CShaderAPIEmpty::GetNearestRenderTargetFormat( ImageFormat fmt ) const
{
	return fmt;
}

// Sets the texture state
void CShaderAPIEmpty::BindTexture( Sampler_t stage, ShaderAPITextureHandle_t textureHandle )
{
#if defined( ANDROID )
	// Our fixed pipeline only samples one texture per draw - only track the
	// base texture stage (0); lightmaps/other stages are ignored for now.
	if ( stage == 0 )
		g_BoundTexture0 = textureHandle;
#endif
}

void CShaderAPIEmpty::ClearColor3ub( unsigned char r, unsigned char g, unsigned char b )
{
}

void CShaderAPIEmpty::ClearColor4ub( unsigned char r, unsigned char g, unsigned char b, unsigned char a )
{
}

// Indicates we're going to be modifying this texture
// TexImage2D, TexSubImage2D, TexWrap, TexMinFilter, and TexMagFilter
// all use the texture specified by this function.
void CShaderAPIEmpty::ModifyTexture( ShaderAPITextureHandle_t textureHandle )
{
#if defined( ANDROID )
	g_ModifyTextureHandle = textureHandle;
#endif
}

// Texture management methods
void CShaderAPIEmpty::TexImage2D( int level, int cubeFace, ImageFormat dstFormat, int zOffset, int width, int height,
						 ImageFormat srcFormat, bool bSrcIsTiled, void *imageData )
{
#if defined( ANDROID )
	if ( !imageData || level != 0 || width <= 0 || height <= 0 || !IsValidTextureHandle( g_ModifyTextureHandle ) )
		return;

	VulkanTexture &tex = GetTexture( g_ModifyTextureHandle );
	tex.width = width;
	tex.height = height;
	if ( !EnsureTextureImage( tex ) )
		return;

	std::vector<unsigned char> rgba;
	rgba.resize( (size_t)width * height * 4 );
	if ( !ConvertToRgba8( srcFormat, (const unsigned char *)imageData, rgba.data(), width * height ) )
		return; // compressed/unrecognized format - stays on the white fallback

	if ( UploadRgba8ToImage( tex.image, width, height, rgba.data() ) )
		tex.hasData = true;
#endif
}

void CShaderAPIEmpty::TexSubImage2D( int level, int cubeFace, int xOffset, int yOffset, int zOffset, int width, int height,
						 ImageFormat srcFormat, int srcStride, bool bSrcIsTiled, void *imageData )
{
}

void CShaderAPIEmpty::TexImageFromVTF( IVTFTexture *pVTF, int iVTFFrame )
{
}

bool CShaderAPIEmpty::TexLock( int level, int cubeFaceID, int xOffset, int yOffset,
								int width, int height, CPixelWriter& writer )
{
#if defined( ANDROID )
	// Used heavily by the lightmap system - callers gate level-load
	// completion on this call actually succeeding, so it must return true
	// (see the TexUnlock() comment for why the data isn't discarded
	// anymore). writer always gets an RGBA8 scratch buffer regardless of
	// the texture's real destination format; TexUnlock() uploads that
	// buffer as-is (only correct for xOffset==yOffset==0, i.e. whole-image
	// locks, which is the common case for lightmap pages).
	if ( width <= 0 || height <= 0 )
		return false;

	int stride = width * 4;
	size_t needed = (size_t)stride * (size_t)height;
	if ( needed > g_nTexLockScratchSize )
	{
		free( g_pTexLockScratch );
		g_pTexLockScratch = (unsigned char *)malloc( needed );
		g_nTexLockScratchSize = g_pTexLockScratch ? needed : 0;
	}
	if ( !g_pTexLockScratch )
		return false;

	writer.SetPixelMemory( IMAGE_FORMAT_RGBA8888, g_pTexLockScratch, stride );
	g_nTexLockWidth = width;
	g_nTexLockHeight = height;
	return true;
#else
	return false;
#endif
}

void CShaderAPIEmpty::TexUnlock( )
{
#if defined( ANDROID )
	if ( !g_pTexLockScratch || g_nTexLockWidth <= 0 || g_nTexLockHeight <= 0 || !IsValidTextureHandle( g_ModifyTextureHandle ) )
		return;

	VulkanTexture &tex = GetTexture( g_ModifyTextureHandle );
	if ( tex.width <= 0 )
	{
		tex.width = g_nTexLockWidth;
		tex.height = g_nTexLockHeight;
	}
	if ( !EnsureTextureImage( tex ) )
		return;

	if ( UploadRgba8ToImage( tex.image, g_nTexLockWidth, g_nTexLockHeight, g_pTexLockScratch ) )
		tex.hasData = true;
#endif
}


// These are bound to the texture, not the texture environment
void CShaderAPIEmpty::TexMinFilter( ShaderTexFilterMode_t texFilterMode )
{
}

void CShaderAPIEmpty::TexMagFilter( ShaderTexFilterMode_t texFilterMode )
{
}

void CShaderAPIEmpty::TexWrap( ShaderTexCoordComponent_t coord, ShaderTexWrapMode_t wrapMode )
{
}

void CShaderAPIEmpty::TexSetPriority( int priority )
{
}

ShaderAPITextureHandle_t CShaderAPIEmpty::CreateTexture( 
	int width, 
	int height,
	int depth,
	ImageFormat dstImageFormat, 
	int numMipLevels, 
	int numCopies, 
	int flags, 
	const char *pDebugName,
	const char *pTextureGroupName )
{
#if defined( ANDROID )
	// The Vulkan image itself is created lazily (EnsureTextureImage(), on
	// first TexImage2D/TexUnlock upload) since the device may not exist yet.
	VulkanTexture tex;
	tex.width = width;
	tex.height = height;
	g_Textures.push_back( tex );
	return (ShaderAPITextureHandle_t)g_Textures.size();
#else
	return 0;
#endif
}

// Create a multi-frame texture (equivalent to calling "CreateTexture" multiple times, but more efficient)
void CShaderAPIEmpty::CreateTextures( 
							ShaderAPITextureHandle_t *pHandles,
							int count,
							int width, 
							int height,
							int depth,
							ImageFormat dstImageFormat, 
							int numMipLevels, 
							int numCopies, 
							int flags, 
							const char *pDebugName,
							const char *pTextureGroupName )
{
	for ( int k = 0; k < count; ++ k )
	{
#if defined( ANDROID )
		VulkanTexture tex;
		tex.width = width;
		tex.height = height;
		g_Textures.push_back( tex );
		pHandles[ k ] = (ShaderAPITextureHandle_t)g_Textures.size();
#else
		pHandles[ k ] = 0;
#endif
	}
}


ShaderAPITextureHandle_t CShaderAPIEmpty::CreateDepthTexture( ImageFormat renderFormat, int width, int height, const char *pDebugName, bool bTexture )
{
	return 0;
}

void CShaderAPIEmpty::DeleteTexture( ShaderAPITextureHandle_t textureHandle )
{
}

bool CShaderAPIEmpty::IsTexture( ShaderAPITextureHandle_t textureHandle )
{
	return true;
}

bool CShaderAPIEmpty::IsTextureResident( ShaderAPITextureHandle_t textureHandle )
{
	return false;
}

// stuff that isn't to be used from within a shader
void CShaderAPIEmpty::ClearBuffers( bool bClearColor, bool bClearDepth, bool bClearStencil, int renderTargetWidth, int renderTargetHeight )
{
#if defined( ANDROID )
	// The render pass always clears on load (see CreateRenderPass() in
	// vr_xr_vulkan.cpp) - this just makes sure a scene/command buffer is
	// open before any of the draws that follow it.
	VRXR_BeginSceneIfNeeded();
#endif
}

void CShaderAPIEmpty::ClearBuffersObeyStencil( bool bClearColor, bool bClearDepth )
{
}

void CShaderAPIEmpty::ClearBuffersObeyStencilEx( bool bClearColor, bool bClearAlpha, bool bClearDepth )
{
}

void CShaderAPIEmpty::PerformFullScreenStencilOperation( void )
{
}

void CShaderAPIEmpty::SetScissorRect( const int nLeft, const int nTop, const int nRight, const int nBottom, const bool bEnableScissor )
{
}

void CShaderAPIEmpty::ReadPixels( int x, int y, int width, int height, unsigned char *data, ImageFormat dstFormat )
{
}

void CShaderAPIEmpty::ReadPixels( Rect_t *pSrcRect, Rect_t *pDstRect, unsigned char *data, ImageFormat dstFormat, int nDstStride )
{
}

void CShaderAPIEmpty::FlushHardware()
{
}

void CShaderAPIEmpty::ResetRenderState( bool bFullReset )
{
}

// Set the number of bone weights
void CShaderAPIEmpty::SetNumBoneWeights( int numBones )
{
}

void CShaderAPIEmpty::EnableHWMorphing( bool bEnable )
{
}

// Selection mode methods
int CShaderAPIEmpty::SelectionMode( bool selectionMode )
{
	return 0;
}

void CShaderAPIEmpty::SelectionBuffer( unsigned int* pBuffer, int size )
{
}

void CShaderAPIEmpty::ClearSelectionNames( )
{
}

void CShaderAPIEmpty::LoadSelectionName( int name )
{
}

void CShaderAPIEmpty::PushSelectionName( int name )
{
}

void CShaderAPIEmpty::PopSelectionName()
{
}


// Use this to get the mesh builder that allows us to modify vertex data
CMeshBuilder* CShaderAPIEmpty::GetVertexModifyBuilder()
{
	return 0;
}

// Board-independent calls, here to unify how shaders set state
// Implementations should chain back to IShaderUtil->BindTexture(), etc.

// Use this to begin and end the frame
void CShaderAPIEmpty::BeginFrame()
{
}

void CShaderAPIEmpty::EndFrame()
{
}

// returns the current time in seconds....
double CShaderAPIEmpty::CurrentTime() const
{
	return Sys_FloatTime();
}

// Get the current camera position in world space.
void CShaderAPIEmpty::GetWorldSpaceCameraPosition( float * pPos ) const
{
}

void CShaderAPIEmpty::ForceHardwareSync( void )
{
}

void CShaderAPIEmpty::SetClipPlane( int index, const float *pPlane )
{
}

void CShaderAPIEmpty::EnableClipPlane( int index, bool bEnable )
{
}

void CShaderAPIEmpty::SetFastClipPlane( const float *pPlane )
{
}

void CShaderAPIEmpty::EnableFastClip( bool bEnable )
{
}

int CShaderAPIEmpty::GetCurrentNumBones( void ) const
{
	return 0;
}

bool CShaderAPIEmpty::IsHWMorphingEnabled( void ) const
{
	return false;
}

int CShaderAPIEmpty::GetCurrentLightCombo( void ) const
{
	return 0;
}

void CShaderAPIEmpty::GetDX9LightState( LightState_t *state ) const
{
	state->m_nNumLights = 0;
	state->m_bAmbientLight = false;
	state->m_bStaticLightVertex = false;
	state->m_bStaticLightTexel = false;
}

MaterialFogMode_t CShaderAPIEmpty::GetCurrentFogType( void ) const
{
	return MATERIAL_FOG_NONE;
}

void CShaderAPIEmpty::RecordString( const char *pStr )
{
}

bool CShaderAPIEmpty::ReadPixelsFromFrontBuffer() const
{
	return true;
}

bool CShaderAPIEmpty::PreferDynamicTextures() const
{
	return false;
}

bool CShaderAPIEmpty::PreferReducedFillrate() const
{ 
	return false; 
}

bool CShaderAPIEmpty::HasProjectedBumpEnv() const
{
	return true;
}

int  CShaderAPIEmpty::GetCurrentDynamicVBSize( void )
{
	return 0;
}

void CShaderAPIEmpty::DestroyVertexBuffers( bool bExitingLevel )
{
}

void CShaderAPIEmpty::EvictManagedResources()
{
}

void CShaderAPIEmpty::SetTextureTransformDimension( TextureStage_t textureStage, int dimension, bool projected )
{
}

void CShaderAPIEmpty::SetBumpEnvMatrix( TextureStage_t textureStage, float m00, float m01, float m10, float m11 )
{
}

void CShaderAPIEmpty::SyncToken( const char *pToken )
{
}
