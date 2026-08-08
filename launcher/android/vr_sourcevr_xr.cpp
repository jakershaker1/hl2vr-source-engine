//========= Copyright Valve Corporation, All rights reserved. ============//
//
// OpenXR-backed ISourceVirtualReality implementation. The engine's own
// stereo camera/rendering code (game/client/view.cpp, viewrender.cpp,
// client_virtualreality.cpp) already exists and is fully functional - it's
// been dormant this whole port because g_pSourceVR was always NULL. This
// file is the missing backend: it answers the engine's per-frame pose/
// projection/viewport questions using data queried from OpenXR (see
// vr_xr_gles.cpp), instead of OpenVR/SteamVR like the desktop-only
// reference implementation at sourcevr/sourcevirtualreality.cpp (kept
// there as a coordinate-conversion reference - the OpenVR->Source
// coordinate transform below is copied from it unchanged, since OpenXR
// and OpenVR use the identical X=right/Y=up/Z=backward convention).
//
// GetRenderTarget() intentionally returns NULL for both eyes: Push3DView()
// falls back to "whatever the current render target is" (engine/gl_rmain.cpp)
// when given NULL, which is the real backbuffer. Combined with
// GetViewportBounds() splitting that backbuffer into left/right halves,
// both eyes render side-by-side into one ordinary D3D9 backbuffer - no
// offscreen materialsystem render targets, no need to get a raw GL texture
// out of an ITexture (there's no such wrapper in this codebase). The
// side-by-side backbuffer is what appframework/sdlmgr.cpp's ShowPixels
// hook (see g_pfnHL2VR_PresentFrame) hands to vr_xr_gles.cpp's
// PresentFrame(), which splits it back into two separate eye images for
// the OpenXR swapchains.
//
//===========================================================================//

#include "sourcevr/isourcevirtualreality.h"
#include "vr_xr_gles.h"
#include "appframework/ilaunchermgr.h"
#include "materialsystem/imaterialsystem.h"
#include "materialsystem/imaterial.h"
#include "materialsystem/itexture.h"
#include "materialsystem/MaterialSystemUtil.h"
#include "tier1/KeyValues.h"

#include <android/log.h>
#include <math.h>

#define LOGI( ... ) __android_log_print( ANDROID_LOG_INFO, "hl2vr.vr", __VA_ARGS__ )

extern ILauncherMgr *g_pLauncherMgr;

namespace
{
	// Builds a VMatrix from a 3x4 row-major array (last column = translation),
	// matching sourcevr/sourcevirtualreality.cpp's VMatrixFrom34 helper.
	VMatrix VMatrixFrom34( const float v[3][4] )
	{
		return VMatrix(
			v[0][0], v[0][1], v[0][2], v[0][3],
			v[1][0], v[1][1], v[1][2], v[1][3],
			v[2][0], v[2][1], v[2][2], v[2][3],
			0,       0,       0,       1       );
	}

	// Standard unit-quaternion -> 3x3 rotation matrix, packed into the same
	// row-major 3x4 layout VMatrixFrom34 expects (translation in column 3).
	VMatrix PoseToMatrix( const VRXR_Pose_t &pose )
	{
		float x = pose.qx, y = pose.qy, z = pose.qz, w = pose.qw;
		float xx = x*x, yy = y*y, zz = z*z;
		float xy = x*y, xz = x*z, yz = y*z;
		float wx = w*x, wy = w*y, wz = w*z;

		float m[3][4] = {
			{ 1-2*(yy+zz), 2*(xy-wz),   2*(xz+wy),   pose.px },
			{ 2*(xy+wz),   1-2*(xx+zz), 2*(yz-wx),   pose.py },
			{ 2*(xz-wy),   2*(yz+wx),   1-2*(xx+yy), pose.pz },
		};
		return VMatrixFrom34( m );
	}

	// From OpenXR/OpenVR: X=right, Y=up, Z=backwards, scale is meters.
	// To Source: X=forwards, Y=left, Z=up, scale is inches.
	// Copied unchanged from sourcevr/sourcevirtualreality.cpp's
	// OpenVRToSourceCoordinateSystem - OpenXR uses the identical convention,
	// so the same fixed change-of-basis applies.
	VMatrix XrToSourceCoordinateSystem( const VMatrix &v )
	{
		const float inchesPerMeter = 39.3700787f;
		const vec_t (*m)[4] = v.m;
		return VMatrix(
			 m[2][2],  m[2][0], -m[2][1], -m[2][3] * inchesPerMeter,
			 m[0][2],  m[0][0], -m[0][1], -m[0][3] * inchesPerMeter,
			-m[1][2], -m[1][0],  m[1][1],  m[1][3] * inchesPerMeter,
			-m[3][2], -m[3][0],  m[3][1],  m[3][3] );
	}

	// Off-axis projection from OpenXR's raw per-eye tangent extents.
	//
	// This is Khronos's own XrMatrix4x4f_CreateProjection (OpenXR-SDK
	// src/common/xr_linear.h), D3D branch - i.e. clip space with +Y up and a
	// [0,1] Z range, which is what Source's D3D9 renderer expects. Taking the
	// OpenXR tangents directly, rather than reshaping them to fit
	// sourcevr/sourcevirtualreality.cpp's OpenVR-shaped
	// ComposeProjectionTransform(left,right,top,bottom), just avoids a
	// conversion step - the two disagree on the sign of the vertical
	// off-centre term, and matching the OpenXR reference exactly is the
	// safer of the two to reason about.
	//
	// For what it's worth that sign is moot on this headset: its vertical
	// frustum is exactly symmetric (measured angleUp=+0.91751,
	// angleDown=-0.91751), so tanUp+tanDown is 0 and p[1][2] vanishes either
	// way. The horizontal frustum IS strongly canted (p[0][2] ~= -/+0.253,
	// mirrored per eye) and that term does matter.
	void ComposeProjectionTransform( float tanLeft, float tanRight, float tanUp, float tanDown,
		float zNear, float zFar, float fovScale, VMatrix *pmProj )
	{
		if ( fovScale != 1.0f && fovScale > 0.f )
		{
			float fFovScaleAdjusted = tanf( atanf( tanUp ) / fovScale ) / tanUp;
			tanLeft *= fFovScaleAdjusted;
			tanRight *= fFovScaleAdjusted;
			tanUp *= fFovScaleAdjusted;
			tanDown *= fFovScaleAdjusted;
		}

		const float tanWidth = tanRight - tanLeft;
		// tanUp - tanDown (not the reverse): +Y-up clip space, per the
		// reference's OpenGL/D3D/Metal branch.
		const float tanHeight = tanUp - tanDown;
		const float idz = 1.0f / ( zFar - zNear );

		float (*p)[4] = pmProj->m;
		p[0][0] = 2.0f/tanWidth; p[0][1] = 0;              p[0][2] = ( tanRight + tanLeft ) / tanWidth;  p[0][3] = 0;
		p[1][0] = 0;             p[1][1] = 2.0f/tanHeight; p[1][2] = ( tanUp + tanDown ) / tanHeight;    p[1][3] = 0;
		p[2][0] = 0;             p[2][1] = 0;              p[2][2] = -zFar*idz;                          p[2][3] = -zFar*zNear*idz;
		p[3][0] = 0;             p[3][1] = 0;              p[3][2] = -1.0f;                              p[3][3] = 0;
	}

	class CSourceVirtualRealityXR : public ISourceVirtualReality
	{
	public:
		bool Connect( CreateInterfaceFn factory ) override
		{
			// Stashed so the HUD materials can be created lazily - see
			// EnsureHudMaterials.
			if ( factory )
				m_pMaterialSystem = (IMaterialSystem *)factory( MATERIAL_SYSTEM_INTERFACE_VERSION, NULL );
			return true;
		}
		void Disconnect() override { m_pMaterialSystem = NULL; }
		void *QueryInterface( const char *pInterfaceName ) override
		{
			if ( !Q_stricmp( pInterfaceName, SOURCE_VIRTUAL_REALITY_INTERFACE_VERSION ) )
				return this;
			return NULL;
		}
		InitReturnVal_t Init() override { return INIT_OK; }
		void Shutdown() override {}

		bool ShouldRunInVR() override { return VRXR_IsRunning(); }
		bool IsHmdConnected() override { return true; }

		void GetViewportBounds( VREye eEye, int *pnX, int *pnY, int *pnWidth, int *pnHeight ) override
		{
			// Side-by-side: left eye gets the left half of the window, right
			// eye the right half - see the file header comment. Some callers
			// (window/mode setup, not per-eye rendering) only want the size
			// and pass NULL for pnX/pnY - matches sourcevr/sourcevirtualreality.cpp's
			// reference implementation's null-checking convention.
			uint w = 0, h = 0;
			if ( g_pLauncherMgr )
				g_pLauncherMgr->DisplayedSize( w, h );
			if ( w == 0 ) w = 1280;
			if ( h == 0 ) h = 720;

			int halfWidth = (int)w / 2;
			if ( pnWidth ) *pnWidth = halfWidth;
			if ( pnHeight ) *pnHeight = (int)h;
			if ( pnY ) *pnY = 0;
			if ( pnX ) *pnX = ( eEye == VREye_Left ) ? 0 : halfWidth;
		}

		bool DoDistortionProcessing( VREye eEye ) override
		{
			// The OpenXR runtime's own compositor handles lens distortion -
			// nothing for the app to do.
			return true;
		}

		bool CompositeHud( VREye eEye, float ndcHudBounds[4], bool bDoUndistort, bool bBlackout, bool bTranslucent ) override
		{
			// vr_hud_never_overlay/vr_render_hud_in_world (set at startup, see
			// launcher.cpp) keep the engine off this path entirely for now.
			return false;
		}

		VMatrix GetMideyePose() override
		{
			VRXR_Pose_t left = VRXR_GetEyePose( 0 );
			VRXR_Pose_t right = VRXR_GetEyePose( 1 );
			VRXR_Pose_t mid;
			mid.px = ( left.px + right.px ) * 0.5f;
			mid.py = ( left.py + right.py ) * 0.5f;
			mid.pz = ( left.pz + right.pz ) * 0.5f;
			// Both eyes report ~the same orientation on every runtime we care
			// about - left eye's is a fine approximation of "head" orientation.
			mid.qx = left.qx; mid.qy = left.qy; mid.qz = left.qz; mid.qw = left.qw;

			return XrToSourceCoordinateSystem( PoseToMatrix( mid ) );
		}

		bool SampleTrackingState( float PlayerGameFov, float fPredictionSeconds ) override
		{
			return VRXR_BeginFrame();
		}

		bool GetDisplayBounds( VRRect_t *pRect ) override
		{
			uint w = 0, h = 0;
			if ( g_pLauncherMgr )
				g_pLauncherMgr->DisplayedSize( w, h );
			pRect->nX = 0;
			pRect->nY = 0;
			pRect->nWidth = (int32)w;
			pRect->nHeight = (int32)h;
			return true;
		}

		bool GetEyeProjectionMatrix( VMatrix *pResult, VREye eEye, float zNear, float zFar, float fovScale ) override
		{
			VRXR_Fov_t fov = VRXR_GetEyeFov( eEye == VREye_Left ? 0 : 1 );

			// Raw OpenXR tangents, passed through unmodified - angleUp/angleRight
			// positive, angleDown/angleLeft negative. ComposeProjectionTransform
			// consumes that convention directly (see the note there about why
			// pre-negating to fit OpenVR's convention was the bug).
			ComposeProjectionTransform(
				tanf( fov.angleLeft ), tanf( fov.angleRight ),
				tanf( fov.angleUp ), tanf( fov.angleDown ),
				zNear, zFar, fovScale, pResult );
			return true;
		}

		VMatrix GetMidEyeFromEye( VREye eEye ) override
		{
			VRXR_Pose_t left = VRXR_GetEyePose( 0 );
			VRXR_Pose_t right = VRXR_GetEyePose( 1 );
			VRXR_Pose_t mid;
			mid.px = ( left.px + right.px ) * 0.5f;
			mid.py = ( left.py + right.py ) * 0.5f;
			mid.pz = ( left.pz + right.pz ) * 0.5f;
			mid.qx = left.qx; mid.qy = left.qy; mid.qz = left.qz; mid.qw = left.qw;

			VRXR_Pose_t eye = ( eEye == VREye_Left ) ? left : right;

			// Relative transform in OpenXR space first, then convert through
			// the same fixed change-of-basis - valid because that conversion
			// is a similarity transform (see file header).
			VMatrix midEyeFromWorld = PoseToMatrix( mid ).InverseTR();
			VMatrix midEyeFromEyeXr = midEyeFromWorld * PoseToMatrix( eye );
			return XrToSourceCoordinateSystem( midEyeFromEyeXr );
		}

		int GetVRModeAdapter() override { return -1; }

		bool WillDriftInYaw() override { return false; }

		// Supplies the two materials CClientVirtualReality::RenderHUDQuad()
		// looks up to draw the HUD/menu quad in world space.
		//
		// They don't exist in retail HL2 content: "inworldui" appears in none
		// of the shipped VPKs (it came with the SDK/VR-era builds), so
		// FindMaterial returns the error material and the quad draws as the
		// magenta/black checkerboard. The engine asserts !IsErrorMaterial()
		// there, but asserts compile out in release.
		//
		// Both just sample _rt_gui, the render target
		// CClientVirtualReality::DrawMainMenu() paints the UI panels into, so
		// they're trivial to supply procedurally rather than shipping content.
		//
		// Called lazily rather than from CreateRenderTargets, because the
		// engine only calls that `if ( UseVR() )` (matsys_interface.cpp) -
		// and UseVR() is ShouldRunInVR(), which is false until the OpenXR
		// session is actually running. The session is created lazily on the
		// first present, long after render targets are set up, so that hook
		// never fires for us.
		void EnsureHudMaterials()
		{
			if ( m_bHudMaterialsReady || !m_pMaterialSystem )
				return;

			struct { const char *pName; bool bTranslucent; } kMaterials[] = {
				{ "vgui/inworldui",        true  },
				{ "vgui/inworldui_opaque", false },
			};

			for ( int i = 0; i < ARRAYSIZE( kMaterials ); i++ )
			{
				// Created unconditionally rather than only when the existing
				// lookup is an error material. FindMaterial() reports
				// IsErrorMaterial()==false for these even though they are in
				// none of the shipped VPKs, so that test skipped creation and
				// left the quad drawing the missing-texture checkerboard.
				//
				// CreateMaterial takes ownership of the KeyValues.
				KeyValues *pVMT = new KeyValues( "UnlitGeneric" );
				pVMT->SetString( "$basetexture", "_rt_gui" );
				pVMT->SetInt( "$translucent", kMaterials[i].bTranslucent ? 1 : 0 );
				pVMT->SetInt( "$vertexcolor", 0 );
				pVMT->SetInt( "$vertexalpha", 0 );
				pVMT->SetInt( "$ignorez", 0 );
				IMaterial *pMat = m_pMaterialSystem->CreateMaterial( kMaterials[i].pName, pVMT );

				// _rt_gui is created just above, so anything that resolved
				// this material earlier bound a $basetexture that did not
				// exist yet - re-resolve it now.
				if ( pMat )
					pMat->Refresh();

			}

			m_bHudMaterialsReady = true;
		}

		void CreateRenderTargets( IMaterialSystem *pMaterialSystem ) override
		{
			// Called from InitWellKnownRenderTargets (engine/matsys_interface.cpp)
			// inside the material system's Begin/EndRenderTargetAllocation
			// phase - render targets have to be created here, not lazily
			// mid-frame.
			//
			// No per-eye *scene* render targets on purpose: GetRenderTarget()
			// returns NULL so the engine renders into the real backbuffer (see
			// the file header). What we do create is _rt_gui, the target
			// CClientVirtualReality::DrawMainMenu() paints the UI panels into
			// and that the in-world HUD quad's material samples.
			if ( pMaterialSystem && !m_pMaterialSystem )
				m_pMaterialSystem = pMaterialSystem;

			if ( !m_pMaterialSystem )
				return;

			if ( !m_GuiRenderTarget.IsValid() )
			{
				// Sized to one eye rather than the reference implementation's
				// fixed 640x480: DrawMainMenu sizes the VGUI panels from
				// GetScreenSize() (the per-eye viewport) but paints them into
				// this target's viewport, so anything smaller clips the menu
				// to its top-left corner.
				uint w = 0, h = 0;
				g_pLauncherMgr->DisplayedSize( w, h );
				int rtWidth = ( w > 0 ) ? (int)w / 2 : 640;   // DisplayedSize is both eyes
				int rtHeight = ( h > 0 ) ? (int)h : 480;

				m_GuiRenderTarget.Init( m_pMaterialSystem->CreateNamedRenderTargetTextureEx2(
					"_rt_gui",
					rtWidth, rtHeight, RT_SIZE_LITERAL,
					m_pMaterialSystem->GetBackBufferFormat(),
					MATERIAL_RT_DEPTH_SHARED,
					TEXTUREFLAGS_CLAMPS | TEXTUREFLAGS_CLAMPT,
					0 ) );

				LOGI( "created _rt_gui render target %dx%d (valid=%d)",
					rtWidth, rtHeight, (int)m_GuiRenderTarget.IsValid() );

			}

			EnsureHudMaterials();
		}
		void ShutdownRenderTargets() override {}
		ITexture *GetRenderTarget( VREye eEye, EWhichRenderTarget eWhich ) override { return NULL; }

		void GetRenderTargetFrameBufferDimensions( int &nWidth, int &nHeight ) override
		{
			uint w = 0, h = 0;
			if ( g_pLauncherMgr )
				g_pLauncherMgr->DisplayedSize( w, h );
			nWidth = (int)w;
			nHeight = (int)h;
		}

		bool Activate() override { return true; }
		void Deactivate() override {}

		bool ShouldForceVRMode() override { return true; }
		void SetShouldForceVRMode() override {}

	private:
		IMaterialSystem *m_pMaterialSystem = NULL;
		bool m_bHudMaterialsReady = false;
		CTextureReference m_GuiRenderTarget;
	};

	CSourceVirtualRealityXR g_SourceVirtualRealityXR;
}

void *CreateSourceVirtualRealityXR()
{
	return static_cast<ISourceVirtualReality *>( &g_SourceVirtualRealityXR );
}
