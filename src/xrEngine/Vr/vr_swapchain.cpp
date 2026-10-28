#ifdef XR_VR_TEST
#include "../../../tests/vr/support.h"
#else
#include "../stdafx.h"
#endif

#ifdef VR_ENABLED

#include "vr_swapchain.h"

namespace vr
{
	bool CreateEyeSwapchain(ID3D11Device* device,
	                        XrSession session,
	                        const XrViewConfigurationView& config,
	                        int64_t format,
	                        float render_scale,
	                        int32_t width,
	                        int32_t height,
	                        EyeSwapchain& out)
	{
		if (!device)
			return false;

		out.format = format;
		out.width = (width > 0) ? width : (int32_t)((float)config.recommendedImageRectWidth * render_scale);
		out.height = (height > 0) ? height : (int32_t)((float)config.recommendedImageRectHeight * render_scale);
		if (out.width < 1) out.width = 1;
		if (out.height < 1) out.height = 1;

		XrSwapchainCreateInfo ci{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
		ci.arraySize = 1;
		ci.format = format;
		ci.width = out.width;
		ci.height = out.height;
		ci.mipCount = 1;
		ci.faceCount = 1;
		ci.sampleCount = 1;
		ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;

		VR_XR_CHECK(xrCreateSwapchain(session, &ci, &out.handle), "xrCreateSwapchain");

		uint32_t image_count = 0;
		VR_XR_CHECK(xrEnumerateSwapchainImages(out.handle, 0, &image_count, nullptr),
			"xrEnumerateSwapchainImages(count)");
		if (image_count == 0)
		{
			Msg("! [VR] swapchain reported zero images");
			return false;
		}

		out.images.assign(image_count, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
		VR_XR_CHECK(xrEnumerateSwapchainImages(out.handle, image_count, &image_count,
			reinterpret_cast<XrSwapchainImageBaseHeader*>(out.images.data())),
			"xrEnumerateSwapchainImages(data)");

		out.views.assign(image_count, nullptr);
		for (uint32_t i = 0; i < image_count; ++i)
		{
			ID3D11Texture2D* tex = out.images[i].texture;
			if (!tex)
			{
				Msg("! [VR] swapchain image %u has a null texture", i);
				return false;
			}

            // OpenXR D3D11 images are typeless. Describe the view explicitly:
            // engine SDR shaders output gamma-encoded values, so write through
            // UNORM while the compositor reads the swapchain as sRGB.
            D3D11_TEXTURE2D_DESC td{};
            tex->GetDesc(&td);
            D3D11_RENDER_TARGET_VIEW_DESC rd{};
            rd.Format = static_cast<DXGI_FORMAT>(format);
            if (rd.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
                rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            else if (rd.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
                rd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            if (td.ArraySize > 1)
            {
                rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
                rd.Texture2DArray.ArraySize = 1;
            }
            else
                rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            const HRESULT hr = device->CreateRenderTargetView(tex, &rd, &out.views[i]);
            if (FAILED(hr))
            {
                Msg("! [VR] CreateRenderTargetView failed (0x%08x) tex=%p fmt=%d rtvfmt=%d array=%u mips=%u samples=%u bind=0x%x",
                    (unsigned)hr, tex, (int)td.Format, (int)rd.Format,
                    td.ArraySize, td.MipLevels, td.SampleDesc.Count, td.BindFlags);
                return false;
            }
		}

		Msg("* [VR] eye swapchain created %dx%d, %u images, fmt=%lld",
			out.width, out.height, image_count, (long long)format);
		return true;
	}

	void DestroyEyeSwapchain(EyeSwapchain& sc)
	{
		if (sc.acquired && sc.waited) ReleaseEyeImage(sc);
		for (ID3D11RenderTargetView*& view : sc.views)
			if (view) { view->Release(); view = nullptr; }
		sc.views.clear();
		sc.images.clear();

		if (sc.handle != XR_NULL_HANDLE)
		{
			xrDestroySwapchain(sc.handle);
			sc.handle = XR_NULL_HANDLE;
		}
		sc.acquired = sc.waited = false;
	}

	bool AcquireEyeImage(EyeSwapchain& sc, const char* what)
	{
		if (sc.acquired || !sc.handle) return false;
		XrSwapchainImageAcquireInfo info{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
		VR_XR_CHECK(xrAcquireSwapchainImage(sc.handle, &info, &sc.acquired_index), what);
		sc.acquired = true;
		sc.waited = false;
		if (sc.acquired_index >= sc.images.size()) return false;
		return true;
	}

	bool WaitEyeImage(EyeSwapchain& sc, const char* what)
	{
		XrSwapchainImageWaitInfo info{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
		info.timeout = XR_INFINITE_DURATION;
		if (!sc.acquired) return false;
        const XrResult result = xrWaitSwapchainImage(sc.handle, &info);
        if (result != XR_SUCCESS)
        {
            Msg("! [VR] %s: %s", what, ResultToString(result));
            return false;
        }
        sc.waited = true;
		return true;
	}

    bool ReleaseEyeImage(EyeSwapchain& sc)
    {
        if (!sc.acquired) return true;
        // A failed wait is not permission to release or use a runtime image.
        // On terminal errors Shutdown destroys the swapchain/session instead.
        if (!sc.waited) return false;
        XrSwapchainImageReleaseInfo info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        if (!Succeeded(xrReleaseSwapchainImage(sc.handle, &info), "release swapchain image")) return false;
        sc.acquired = sc.waited = false;
        return true;
    }
}
#endif
