#ifndef xrEngine_vr_swapchain_h
#define xrEngine_vr_swapchain_h
#pragma once

#include "vr_common.h"

#ifdef VR_ENABLED

#include <vector>

namespace vr
{
	// One OpenXR swapchain (one eye) plus the D3D11 views used to render into it.
	struct EyeSwapchain
	{
		XrSwapchain handle = XR_NULL_HANDLE;
		int64_t     format = 0;
		int32_t     width = 0;
		int32_t     height = 0;

		std::vector<XrSwapchainImageD3D11KHR> images;
		std::vector<ID3D11RenderTargetView*>  views;

		uint32_t acquired_index = 0;
		bool     acquired = false;
		bool     waited = false;
	};

	bool CreateEyeSwapchain(ID3D11Device* device,
	                        XrSession session,
	                        const XrViewConfigurationView& config,
	                        int64_t format,
	                        float render_scale,
	                        int32_t width,
	                        int32_t height,
	                        EyeSwapchain& out);

	void DestroyEyeSwapchain(EyeSwapchain& sc);

	bool AcquireEyeImage(EyeSwapchain& sc, const char* what);
	bool WaitEyeImage(EyeSwapchain& sc, const char* what);
	bool ReleaseEyeImage(EyeSwapchain& sc);
}

#endif // VR_ENABLED
#endif // xrEngine_vr_swapchain_h
