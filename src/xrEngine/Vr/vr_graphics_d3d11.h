#ifndef xrEngine_vr_graphics_d3d11_h
#define xrEngine_vr_graphics_d3d11_h
#pragma once

#include "vr_common.h"
#include "vr_swapchain.h"

#ifdef VR_ENABLED

#include <vector>

namespace vr
{
	// Queries the runtime's D3D11 requirements and picks a supported colour format.
	struct GraphicsConfig
	{
		XrGraphicsRequirementsD3D11KHR requirements{ XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR };
		int64_t swapchain_format = 0;
	};

	bool QueryGraphicsRequirements(XrInstance instance, XrSystemId system, GraphicsConfig& out);

	// Builds single-sample eye swapchains at a common runtime-recommended size.
	bool CreateEyeSwapchains(ID3D11Device* device,
	                         XrInstance instance,
	                         XrSession session,
	                         XrSystemId system,
	                         const GraphicsConfig& config,
	                         unsigned width,
	                         unsigned height,
	                         int dxgi_format,
	                         EyeSwapchain out_eyes[2]);

	// Depth swapchains matching the eyes, in the eye depth buffer's format
	// (D24S8) so each frame's depth is a plain copy. False, with nothing
	// created, when the runtime does not offer that format.
	bool CreateDepthSwapchains(XrSession session, int32_t width, int32_t height, EyeSwapchain out[2]);
}

#endif // VR_ENABLED
#endif // xrEngine_vr_graphics_d3d11_h
