#ifdef XR_VR_TEST
#include "../../../tests/vr/support.h"
#else
#include "../stdafx.h"
#endif

#ifdef VR_ENABLED

#include "vr_graphics_d3d11.h"
#include "vr_config.h"

namespace vr
{
	static int64_t PickSwapchainFormat(const std::vector<int64_t>& supported)
	{
		// SDR shaders already encode gamma. The compositor must decode those
		// bytes as sRGB; CreateEyeSwapchain uses a non-sRGB RTV to avoid
		// encoding them a second time on write.
		static const int64_t preferred[] = {
			DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
			DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
		};

		for (int64_t want : preferred)
			for (int64_t have : supported)
				if (have == want)
					return want;

		return 0;
	}

	bool QueryGraphicsRequirements(XrInstance instance, XrSystemId system, GraphicsConfig& out)
	{
		PFN_xrGetD3D11GraphicsRequirementsKHR getReqs = nullptr;
		if (!Succeeded(xrGetInstanceProcAddr(instance, "xrGetD3D11GraphicsRequirementsKHR",
		                                     reinterpret_cast<PFN_xrVoidFunction*>(&getReqs)),
			"xrGetInstanceProcAddr(xrGetD3D11GraphicsRequirementsKHR)"))
			return false;
		if (!getReqs)
		{
			Msg("! [VR] runtime does not expose XR_KHR_D3D11_enable");
			return false;
		}

		VR_XR_CHECK(getReqs(instance, system, &out.requirements), "xrGetD3D11GraphicsRequirementsKHR");
		return true;
	}

	bool CreateEyeSwapchains(ID3D11Device* device,
	                         XrInstance instance,
	                         XrSession session,
	                         XrSystemId system,
	                         const GraphicsConfig& config,
	                         unsigned width,
	                         unsigned height,
	                         int dxgi_format,
	                         EyeSwapchain out_eyes[2])
	{
		// Enumerate the runtime's supported colour formats for this session.
		uint32_t format_count = 0;
		VR_XR_CHECK(xrEnumerateSwapchainFormats(session, 0, &format_count, nullptr),
			"xrEnumerateSwapchainFormats(count)");

		std::vector<int64_t> formats(format_count);
		VR_XR_CHECK(xrEnumerateSwapchainFormats(session, format_count, &format_count, formats.data()),
			"xrEnumerateSwapchainFormats(data)");
		formats.resize(format_count);

        const int64_t format = PickSwapchainFormat(formats);
        if (!format) { Msg("! [VR] no supported sRGB swapchain format for gamma-encoded SDR output"); return false; }

		// View configuration (stereo: exactly two views).
		uint32_t view_count = 0;
		VR_XR_CHECK(xrEnumerateViewConfigurationViews(instance, system,
			XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &view_count, nullptr),
			"xrEnumerateViewConfigurationViews(count)");
		if (view_count != 2)
		{
			Msg("! [VR] runtime reported %u views, expected 2", view_count);
			return false;
		}

		std::vector<XrViewConfigurationView> views(view_count,
			{ XR_TYPE_VIEW_CONFIGURATION_VIEW });
		VR_XR_CHECK(xrEnumerateViewConfigurationViews(instance, system,
			XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, view_count, &view_count, views.data()),
			"xrEnumerateViewConfigurationViews(data)");

        // Reuse one set of deferred intermediates sequentially, at a common
        // eye resolution. Both image rectangles must respect both view limits.
        const uint32_t max_width = std::min(views[0].maxImageRectWidth, views[1].maxImageRectWidth);
        const uint32_t max_height = std::min(views[0].maxImageRectHeight, views[1].maxImageRectHeight);
        const uint32_t eye_width = std::max(1u, std::min(max_width,
            uint32_t(std::max(views[0].recommendedImageRectWidth, views[1].recommendedImageRectWidth) * vr_render_scale)));
        const uint32_t eye_height = std::max(1u, std::min(max_height,
            uint32_t(std::max(views[0].recommendedImageRectHeight, views[1].recommendedImageRectHeight) * vr_render_scale)));
        for (uint32_t i = 0; i < 2; ++i)
            if (!CreateEyeSwapchain(device, session, views[i], format, 1, eye_width, eye_height, out_eyes[i])) return false;
        return true;
    }

	bool CreateDepthSwapchains(XrSession session, int32_t width, int32_t height, EyeSwapchain out[2])
	{
		uint32_t count = 0;
		if (XR_FAILED(xrEnumerateSwapchainFormats(session, 0, &count, nullptr)) || !count) return false;
		std::vector<int64_t> formats(count);
		if (XR_FAILED(xrEnumerateSwapchainFormats(session, count, &count, formats.data()))) return false;
		if (std::find(formats.begin(), formats.begin() + count, int64_t(DXGI_FORMAT_D24_UNORM_S8_UINT)) ==
			formats.begin() + count)
		{
			Msg("* [VR] runtime offers no D24S8 depth swapchain; no depth layer");
			return false;
		}
		for (int eye = 0; eye < 2; ++eye)
		{
			EyeSwapchain& sc = out[eye];
			XrSwapchainCreateInfo ci{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
			ci.usageFlags = XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
			ci.format = DXGI_FORMAT_D24_UNORM_S8_UINT;
			ci.sampleCount = ci.faceCount = ci.arraySize = ci.mipCount = 1;
			ci.width = width; ci.height = height;
			uint32_t images = 0;
			bool ok = XR_SUCCEEDED(xrCreateSwapchain(session, &ci, &sc.handle)) &&
				XR_SUCCEEDED(xrEnumerateSwapchainImages(sc.handle, 0, &images, nullptr)) && images;
			if (ok)
			{
				sc.images.assign(images, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
				ok = XR_SUCCEEDED(xrEnumerateSwapchainImages(sc.handle, images, &images,
					reinterpret_cast<XrSwapchainImageBaseHeader*>(sc.images.data())));
				for (const auto& image : sc.images) ok = ok && image.texture;
			}
			if (!ok)
			{
				Msg("! [VR] depth swapchain creation failed; no depth layer");
				DestroyEyeSwapchain(out[0]); DestroyEyeSwapchain(out[1]);
				return false;
			}
			sc.format = ci.format; sc.width = width; sc.height = height;
		}
		Msg("* [VR] depth swapchains created %dx%d (D24S8)", width, height);
		return true;
	}
}
#endif
