//========= Copyright Valve Corporation, All rights reserved. ============//
//
// OpenXR + OpenGL ES session/swapchain plumbing. See vr_xr_gles.h.
//
//===========================================================================//

#include "vr_xr_gles.h"

#include "appframework/ilaunchermgr.h"

#include <android/log.h>
#include <GLES3/gl32.h>

extern ILauncherMgr *g_pLauncherMgr;
extern void (*g_pfnHL2VR_PresentFrame)( unsigned int glTexture, int width, int height );

#define LOG_TAG "hl2vr.vr"
#define LOGI( ... ) __android_log_print( ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__ )
#define LOGE( ... ) __android_log_print( ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__ )

namespace
{
	bool XrCheck( XrResult result, const char *what )
	{
		if ( XR_SUCCEEDED( result ) )
			return true;
		LOGE( "%s failed: XrResult %d", what, (int)result );
		return false;
	}

	// Swapchains in practice have a small handful of images (2-4); cap
	// generously rather than pull in std::vector.
	const uint32_t kMaxSwapchainImages = 8;

	struct Eye
	{
		XrSwapchain swapchain = XR_NULL_HANDLE;
		uint32_t width = 0, height = 0;
		uint32_t imageCount = 0;
		GLuint images[kMaxSwapchainImages] = {};   // GL textures owned by the runtime
		GLuint fbos[kMaxSwapchainImages] = {};     // one draw-FBO per image, wrapping it
	};

	struct VRState
	{
		XrInstance instance = XR_NULL_HANDLE;
		XrSystemId systemId = XR_NULL_SYSTEM_ID;
		XrSession session = XR_NULL_HANDLE;
		XrSpace localSpace = XR_NULL_HANDLE;
		XrSessionState sessionState = XR_SESSION_STATE_UNKNOWN;
		bool sessionRunning = false;
		bool ready = false;

		Eye eyes[2];

		// A single FBO wrapping whatever source texture is handed to us this
		// call, rebound to point at the new texture each frame - cheaper than
		// creating/destroying an FBO every present.
		GLuint srcFbo = 0;

		// Per-frame state, valid between VRXR_BeginFrame() and the matching
		// PresentFrame()'s xrEndFrame.
		bool frameOpen = false;
		XrFrameState frameState = { XR_TYPE_FRAME_STATE };
		XrView views[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
	};

	VRState g_Vr;

	bool CreateXrSession( VRState &vr )
	{
		PFN_xrGetOpenGLESGraphicsRequirementsKHR xrGetOpenGLESGraphicsRequirementsKHR = NULL;
		xrGetInstanceProcAddr( vr.instance, "xrGetOpenGLESGraphicsRequirementsKHR", (PFN_xrVoidFunction *)&xrGetOpenGLESGraphicsRequirementsKHR );
		if ( !xrGetOpenGLESGraphicsRequirementsKHR )
		{
			LOGE( "xrGetOpenGLESGraphicsRequirementsKHR not available" );
			return false;
		}

		XrGraphicsRequirementsOpenGLESKHR reqs = { XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR };
		if ( !XrCheck( xrGetOpenGLESGraphicsRequirementsKHR( vr.instance, vr.systemId, &reqs ), "xrGetOpenGLESGraphicsRequirementsKHR" ) )
			return false;

		if ( !g_pLauncherMgr )
		{
			LOGE( "CreateXrSession: g_pLauncherMgr not set up yet" );
			return false;
		}

		XrGraphicsBindingOpenGLESAndroidKHR binding = { XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR };
		binding.display = (EGLDisplay)g_pLauncherMgr->GetEglDisplay();
		binding.config = (EGLConfig)g_pLauncherMgr->GetEglConfig();
		binding.context = (EGLContext)g_pLauncherMgr->GetMainContext();
		if ( binding.display == EGL_NO_DISPLAY || binding.context == EGL_NO_CONTEXT )
		{
			LOGE( "CreateXrSession: CSDLMgr's EGL display/context not ready yet" );
			return false;
		}

		XrSessionCreateInfo createInfo = { XR_TYPE_SESSION_CREATE_INFO };
		createInfo.next = &binding;
		createInfo.systemId = vr.systemId;
		if ( !XrCheck( xrCreateSession( vr.instance, &createInfo, &vr.session ), "xrCreateSession" ) )
			return false;

		XrReferenceSpaceCreateInfo spaceInfo = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
		spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
		spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
		return XrCheck( xrCreateReferenceSpace( vr.session, &spaceInfo, &vr.localSpace ), "xrCreateReferenceSpace" );
	}

	bool CreateSwapchains( VRState &vr )
	{
		uint32_t viewCount = 0;
		xrEnumerateViewConfigurationViews( vr.instance, vr.systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, NULL );
		if ( viewCount != 2 )
		{
			LOGE( "Expected a stereo (2-view) configuration, got %u views", viewCount );
			return false;
		}

		XrViewConfigurationView viewConfigViews[2] = { { XR_TYPE_VIEW_CONFIGURATION_VIEW }, { XR_TYPE_VIEW_CONFIGURATION_VIEW } };
		if ( !XrCheck( xrEnumerateViewConfigurationViews( vr.instance, vr.systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
			viewCount, &viewCount, viewConfigViews ), "xrEnumerateViewConfigurationViews" ) )
			return false;

		uint32_t formatCount = 0;
		xrEnumerateSwapchainFormats( vr.session, 0, &formatCount, NULL );
		int64_t formats[64] = {};
		formatCount = formatCount > 64 ? 64 : formatCount;
		xrEnumerateSwapchainFormats( vr.session, formatCount, &formatCount, formats );

		int64_t chosenFormat = formatCount == 0 ? GL_RGBA8 : formats[0];
		for ( int64_t preferred : { (int64_t)GL_SRGB8_ALPHA8, (int64_t)GL_RGBA8 } )
		{
			for ( uint32_t k = 0; k < formatCount; k++ )
			{
				if ( formats[k] == preferred ) { chosenFormat = preferred; break; }
			}
		}

		for ( uint32_t i = 0; i < 2; i++ )
		{
			Eye &eye = vr.eyes[i];
			eye.width = viewConfigViews[i].recommendedImageRectWidth;
			eye.height = viewConfigViews[i].recommendedImageRectHeight;

			XrSwapchainCreateInfo swapchainInfo = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
			swapchainInfo.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
			swapchainInfo.format = chosenFormat;
			swapchainInfo.sampleCount = 1;
			swapchainInfo.width = eye.width;
			swapchainInfo.height = eye.height;
			swapchainInfo.faceCount = 1;
			swapchainInfo.arraySize = 1;
			swapchainInfo.mipCount = 1;
			if ( !XrCheck( xrCreateSwapchain( vr.session, &swapchainInfo, &eye.swapchain ), "xrCreateSwapchain" ) )
				return false;

			uint32_t imageCount = 0;
			xrEnumerateSwapchainImages( eye.swapchain, 0, &imageCount, NULL );
			if ( imageCount > kMaxSwapchainImages )
			{
				LOGE( "Swapchain has %u images, more than the %u we support", imageCount, kMaxSwapchainImages );
				return false;
			}

			XrSwapchainImageOpenGLESKHR xrImages[kMaxSwapchainImages];
			for ( uint32_t j = 0; j < imageCount; j++ )
				xrImages[j] = { XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR };
			if ( !XrCheck( xrEnumerateSwapchainImages( eye.swapchain, imageCount, &imageCount,
				(XrSwapchainImageBaseHeader *)xrImages ), "xrEnumerateSwapchainImages" ) )
				return false;

			eye.imageCount = imageCount;
			for ( uint32_t j = 0; j < imageCount; j++ )
			{
				eye.images[j] = xrImages[j].image;

				glGenFramebuffers( 1, &eye.fbos[j] );
				glBindFramebuffer( GL_DRAW_FRAMEBUFFER, eye.fbos[j] );
				glFramebufferTexture2D( GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, eye.images[j], 0 );
			}
		}

		glBindFramebuffer( GL_DRAW_FRAMEBUFFER, 0 );
		glGenFramebuffers( 1, &vr.srcFbo );
		return true;
	}

	void PollXrEvents( VRState &vr )
	{
		XrEventDataBuffer event;
		while ( true )
		{
			event.type = XR_TYPE_EVENT_DATA_BUFFER;
			event.next = NULL;
			if ( xrPollEvent( vr.instance, &event ) != XR_SUCCESS )
				break;

			if ( event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED )
			{
				XrEventDataSessionStateChanged *stateEvent = (XrEventDataSessionStateChanged *)&event;
				vr.sessionState = stateEvent->state;
				LOGI( "XrSessionState -> %d", (int)vr.sessionState );

				if ( vr.sessionState == XR_SESSION_STATE_READY )
				{
					XrSessionBeginInfo beginInfo = { XR_TYPE_SESSION_BEGIN_INFO };
					beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
					if ( XrCheck( xrBeginSession( vr.session, &beginInfo ), "xrBeginSession" ) )
						vr.sessionRunning = true;
				}
				else if ( vr.sessionState == XR_SESSION_STATE_STOPPING )
				{
					xrEndSession( vr.session );
					vr.sessionRunning = false;
				}
			}
		}
	}

	// Runs xrWaitFrame/xrBeginFrame/xrLocateViews and caches the result.
	// Called once per engine frame from ISourceVirtualReality::SampleTrackingState
	// (see vr_sourcevr_xr.cpp) - i.e. *before* the engine sets up the left/right
	// eye CViewSetups and renders them, so the cached poses/fovs are what
	// actually gets rendered this frame (not stale data from the previous one).
	bool DoBeginFrame( VRState &vr )
	{
		if ( vr.frameOpen )
			return true; // already open this frame

		PollXrEvents( vr );
		if ( !vr.sessionRunning )
			return false;

		XrFrameWaitInfo waitInfo = { XR_TYPE_FRAME_WAIT_INFO };
		vr.frameState = { XR_TYPE_FRAME_STATE };
		if ( !XrCheck( xrWaitFrame( vr.session, &waitInfo, &vr.frameState ), "xrWaitFrame" ) )
			return false;

		XrFrameBeginInfo beginInfo = { XR_TYPE_FRAME_BEGIN_INFO };
		xrBeginFrame( vr.session, &beginInfo );
		vr.frameOpen = true;

		vr.views[0] = { XR_TYPE_VIEW };
		vr.views[1] = { XR_TYPE_VIEW };
		XrViewLocateInfo locateInfo = { XR_TYPE_VIEW_LOCATE_INFO };
		locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
		locateInfo.displayTime = vr.frameState.predictedDisplayTime;
		locateInfo.space = vr.localSpace;
		XrViewState viewState = { XR_TYPE_VIEW_STATE };
		uint32_t viewCountOut = 0;
		xrLocateViews( vr.session, &locateInfo, &viewState, 2, &viewCountOut, vr.views );

		return true;
	}

	// Blits the just-finished side-by-side stereo backbuffer (srcTex, from
	// togles/shaderapidx9's EGL context - the same context this XR session
	// was bound to, so no cross-context sharing is needed) into each eye's
	// swapchain image and submits the frame. The engine's own CViewRender
	// per-eye loop (activated by ISourceVirtualReality::ShouldRunInVR(),
	// see vr_sourcevr_xr.cpp) already rendered left/right into the left/right
	// halves of srcTex via CViewSetup viewport bounds, so this is a real
	// stereo present, not a mono duplicate.
	void PresentFrame( unsigned int srcTex, int width, int height )
	{
		if ( !g_Vr.ready )
			return;

		if ( !DoBeginFrame( g_Vr ) )
			return; // session not running (headset not worn) - nothing to present

		glBindFramebuffer( GL_READ_FRAMEBUFFER, g_Vr.srcFbo );
		glFramebufferTexture2D( GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, srcTex, 0 );

		XrCompositionLayerProjectionView projViews[2] = {};
		uint32_t eyeImageIndex[2] = { 0, 0 };
		const int halfWidth = width / 2;

		for ( int i = 0; i < 2; i++ )
		{
			Eye &eye = g_Vr.eyes[i];

			XrSwapchainImageAcquireInfo acquireInfo = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
			xrAcquireSwapchainImage( eye.swapchain, &acquireInfo, &eyeImageIndex[i] );

			XrSwapchainImageWaitInfo waitImgInfo = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
			waitImgInfo.timeout = XR_INFINITE_DURATION;
			xrWaitSwapchainImage( eye.swapchain, &waitImgInfo );

			// Left eye = left half of srcTex, right eye = right half - matches
			// the side-by-side viewport split CSourceVirtualRealityXR::GetViewportBounds
			// hands the engine.
			const int srcXMin = ( i == 0 ) ? 0 : halfWidth;
			const int srcXMax = ( i == 0 ) ? halfWidth : width;

			// Y-flip on the destination (note the swapped dst Y bounds): the
			// engine renders with D3D9's top-left origin via togles, so the
			// source texture is upside down in GL's bottom-left-origin space.
			// CSDLMgr::ShowPixels' own non-VR blit does exactly the same thing
			// ("note yflip here" in appframework/sdlmgr.cpp) - matching it
			// keeps world and HUD consistently oriented, which flipping in the
			// projection matrix instead would not (that flips the world but
			// leaves screen-space HUD alone).
			glBindFramebuffer( GL_DRAW_FRAMEBUFFER, eye.fbos[eyeImageIndex[i]] );
			glBlitFramebuffer( srcXMin, 0, srcXMax, height, 0, (GLint)eye.height, (GLint)eye.width, 0,
				GL_COLOR_BUFFER_BIT, GL_LINEAR );

			XrSwapchainImageReleaseInfo releaseInfo = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
			xrReleaseSwapchainImage( eye.swapchain, &releaseInfo );

			projViews[i].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
			projViews[i].pose = g_Vr.views[i].pose;
			projViews[i].fov = g_Vr.views[i].fov;
			projViews[i].subImage.swapchain = eye.swapchain;
			projViews[i].subImage.imageRect.offset = { 0, 0 };
			projViews[i].subImage.imageRect.extent = { (int32_t)eye.width, (int32_t)eye.height };
		}

		glBindFramebuffer( GL_READ_FRAMEBUFFER, 0 );
		glBindFramebuffer( GL_DRAW_FRAMEBUFFER, 0 );

		XrCompositionLayerProjection projLayer = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
		projLayer.space = g_Vr.localSpace;
		projLayer.viewCount = 2;
		projLayer.views = projViews;
		const XrCompositionLayerBaseHeader *layers[1] = { (const XrCompositionLayerBaseHeader *)&projLayer };

		XrFrameEndInfo endInfo = { XR_TYPE_FRAME_END_INFO };
		endInfo.displayTime = g_Vr.frameState.predictedDisplayTime;
		endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
		// Submit unconditionally rather than gating on frameState.shouldRender -
		// that's a power-saving hint, not a hard requirement (apps are allowed
		// to submit anyway per spec), and the compositor reported our frame
		// rate as 0 / kept us in Home Space rather than Full Space with the
		// gated version, suggesting shouldRender was false far more than
		// expected and we were regularly submitting zero layers.
		endInfo.layerCount = 1;
		endInfo.layers = layers;

		static int s_presentCount = 0;
		static bool s_loggedShouldRenderFalse = false;
		s_presentCount++;
		if ( !g_Vr.frameState.shouldRender && !s_loggedShouldRenderFalse )
		{
			LOGI( "PresentFrame: frameState.shouldRender was false (present #%d) - submitting anyway", s_presentCount );
			s_loggedShouldRenderFalse = true;
		}
		if ( ( s_presentCount % 200 ) == 0 )
			LOGI( "PresentFrame: heartbeat, present #%d", s_presentCount );

		XrResult endResult = xrEndFrame( g_Vr.session, &endInfo );
		if ( !XR_SUCCEEDED( endResult ) )
			LOGE( "xrEndFrame failed: XrResult %d (present #%d)", (int)endResult, s_presentCount );

		g_Vr.frameOpen = false;
	}
}

namespace
{
	// First-present bootstrap. VRXR_Init() is called from android_main's
	// event-loop thread, but CreateSwapchains() makes real GL calls
	// (glGenFramebuffers/glFramebufferTexture2D to wrap each swapchain image
	// in a draw FBO) and CSDLMgr's EGL context is current on the *engine*
	// thread, not that one. An EGL context can only be current on one thread
	// at a time, so doing this from android_main meant every one of those GL
	// calls hit a thread with no current context and silently no-op'd
	// ("call to OpenGL ES API with no current context" in logcat, right at
	// session-ready time). The eye FBOs came back as garbage names, so each
	// frame's glBlitFramebuffer into the swapchain image quietly did nothing:
	// the runtime received structurally valid frames whose images were never
	// written. That's why the system compositor reported "Frame Rate: 0" /
	// "App GPU Time: 0 ms" and Android XR never dismissed its splash or
	// promoted us out of Home Space, even though xrEndFrame kept returning
	// XR_SUCCESS and the session reached VISIBLE.
	//
	// So: defer session + swapchain creation to the first present, which
	// already runs on the engine's render thread with the context current.
	void PresentFrameBootstrap( unsigned int srcTex, int width, int height )
	{
		g_Vr.ready = CreateXrSession( g_Vr ) && CreateSwapchains( g_Vr );
		if ( !g_Vr.ready )
		{
			LOGE( "VR session setup failed" );
			g_pfnHL2VR_PresentFrame = NULL; // don't retry every frame
			return;
		}

		LOGI( "VR session ready: %ux%u per eye", g_Vr.eyes[0].width, g_Vr.eyes[0].height );
		g_pfnHL2VR_PresentFrame = PresentFrame;
		PresentFrame( srcTex, width, height );
	}
}

bool VRXR_Init( XrInstance instance, XrSystemId systemId )
{
	g_Vr.instance = instance;
	g_Vr.systemId = systemId;

	// Real setup happens on the first present - see PresentFrameBootstrap.
	g_pfnHL2VR_PresentFrame = PresentFrameBootstrap;
	return true;
}

void VRXR_PumpEvents()
{
	if ( g_Vr.ready )
		PollXrEvents( g_Vr );
}

bool VRXR_IsRunning()
{
	return g_Vr.ready && g_Vr.sessionRunning;
}

bool VRXR_BeginFrame()
{
	if ( !g_Vr.ready )
		return false;
	return DoBeginFrame( g_Vr );
}

VRXR_Pose_t VRXR_GetEyePose( int eye )
{
	const XrPosef &pose = g_Vr.views[eye & 1].pose;
	VRXR_Pose_t out;
	out.px = pose.position.x; out.py = pose.position.y; out.pz = pose.position.z;
	out.qx = pose.orientation.x; out.qy = pose.orientation.y; out.qz = pose.orientation.z; out.qw = pose.orientation.w;
	return out;
}

VRXR_Fov_t VRXR_GetEyeFov( int eye )
{
	const XrFovf &fov = g_Vr.views[eye & 1].fov;
	VRXR_Fov_t out;
	out.angleLeft = fov.angleLeft; out.angleRight = fov.angleRight;
	out.angleUp = fov.angleUp; out.angleDown = fov.angleDown;
	return out;
}

void VRXR_Shutdown()
{
	g_pfnHL2VR_PresentFrame = NULL;

	if ( g_Vr.session != XR_NULL_HANDLE )
	{
		if ( g_Vr.sessionRunning )
			xrEndSession( g_Vr.session );
		xrDestroySession( g_Vr.session );
	}
	if ( g_Vr.instance != XR_NULL_HANDLE )
		xrDestroyInstance( g_Vr.instance );

	g_Vr = VRState();
}
