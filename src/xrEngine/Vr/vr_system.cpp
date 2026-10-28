#ifdef XR_VR_TEST
#include "../../../tests/vr/support.h"
#else
#include "../stdafx.h"
#endif
#ifdef VR_ENABLED
#include "vr_system.h"
#include "vr_ui_policy.h"
#include "vr_gameplay_policy.h"
#include "vr_config.h"
#include "vr_tracking.h"
#include "vr_input.h"
#ifndef XR_VR_TEST
#include "../igame_persistent.h"
#include "../Environment.h"
#endif

CVrSystem& CVrSystem::Get() { static CVrSystem system; return system; }
CVrSystem::CVrSystem()
{
    m_body_world.identity(); m_tracking_to_body.identity(); m_head_body.identity();
}
const char* CVrSystem::StateName() const
{
    switch (m_state)
    {
    case XR_SESSION_STATE_IDLE: return "IDLE";
    case XR_SESSION_STATE_READY: return "READY";
    case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED";
    case XR_SESSION_STATE_VISIBLE: return "VISIBLE";
    case XR_SESSION_STATE_FOCUSED: return "FOCUSED";
    case XR_SESSION_STATE_STOPPING: return "STOPPING";
    case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING";
    case XR_SESSION_STATE_EXITING: return "EXITING";
    default: return "UNKNOWN";
    }
}
bool CVrSystem::Check(XrResult result, const char* operation)
{
    if (result == XR_SESSION_LOSS_PENDING || XR_FAILED(result))
    {
        Msg("! [VR] %s: %s (%d)", operation, vr::ResultToString(result), int(result));
        m_lost = true;
        return false;
    }
    return true;
}
bool CVrSystem::CreateInstance()
{
    if (!vr::LoadOpenXR()) return false;
    uint32_t count = 0;
    VR_XR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr), "enumerate extensions");
    std::vector<XrExtensionProperties> extensions(count, {XR_TYPE_EXTENSION_PROPERTIES});
    VR_XR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, count, &count, extensions.data()), "enumerate extensions");
    bool d3d11 = false;
    m_depth_extension = m_hp_extension = false;
    for (const auto& extension : extensions)
    {
        if (!strcmp(extension.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME)) d3d11 = true;
        // Optional: eye depth for reprojection, and the HP Reverb G2's own
        // controller profile (its face buttons are lost through the WMR one).
        if (!strcmp(extension.extensionName, XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME)) m_depth_extension = true;
        if (!strcmp(extension.extensionName, XR_EXT_HP_MIXED_REALITY_CONTROLLER_EXTENSION_NAME)) m_hp_extension = true;
    }
    if (!d3d11) { Msg("! [VR] runtime lacks XR_KHR_D3D11_enable"); return false; }
    const char* enabled[3] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    uint32_t enabled_count = 1;
    if (m_depth_extension) enabled[enabled_count++] = XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME;
    if (m_hp_extension) enabled[enabled_count++] = XR_EXT_HP_MIXED_REALITY_CONTROLLER_EXTENSION_NAME;
    XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
    info.enabledExtensionCount = enabled_count; info.enabledExtensionNames = enabled;
    info.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    xr_strcpy(info.applicationInfo.applicationName, "AnomalyDX11");
    xr_strcpy(info.applicationInfo.engineName, "X-Ray Monolith");
    VR_XR_CHECK(xrCreateInstance(&info, &m_instance), "xrCreateInstance");
    if (!vr::LoadOpenXRInstance(m_instance)) return false;
    XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES};
    if (XR_SUCCEEDED(xrGetInstanceProperties(m_instance, &properties)))
        Msg("* [VR] runtime: %s", properties.runtimeName);
    return true;
}
bool CVrSystem::Prepare()
{
    if (m_instance != XR_NULL_HANDLE && m_system != XR_NULL_SYSTEM_ID) return true;
    m_lost = false;
    if (CreateInstance())
    {
        XrSystemGetInfo info{XR_TYPE_SYSTEM_GET_INFO};
        info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        if (vr::Succeeded(xrGetSystem(m_instance, &info, &m_system), "xrGetSystem(HMD)") &&
            vr::QueryGraphicsRequirements(m_instance, m_system, m_graphics)) return true;
    }
    Shutdown();
    return false;
}
bool CVrSystem::CreateSpaces()
{
    XrReferenceSpaceCreateInfo info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    info.poseInReferenceSpace.orientation.w = 1;
    info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    VR_XR_CHECK(xrCreateReferenceSpace(m_session, &info, &m_local_space), "create LOCAL space");
    info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    if (XR_FAILED(xrCreateReferenceSpace(m_session, &info, &m_stage_space))) m_stage_space = XR_NULL_HANDLE;
    info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    VR_XR_CHECK(xrCreateReferenceSpace(m_session, &info, &m_head_space), "create VIEW space");
    m_origin_mode = vr_tracking_origin;
    m_view_space = m_origin_mode == 1 && m_stage_space ? m_stage_space : m_local_space;
    return true;
}
bool CVrSystem::Initialize(ID3D11Device* device, unsigned width, unsigned height, int format)
{
    if (!device || !Prepare()) return false;
    IDXGIDevice* dxgi = nullptr;
    IDXGIAdapter* adapter = nullptr;
    DXGI_ADAPTER_DESC description{};
    bool compatible = SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi)));
    if (compatible) compatible = SUCCEEDED(dxgi->GetAdapter(&adapter));
    if (compatible) compatible = SUCCEEDED(adapter->GetDesc(&description));
    if (adapter) adapter->Release();
    if (dxgi) dxgi->Release();
    const auto& required = m_graphics.requirements;
    compatible = compatible && description.AdapterLuid.HighPart == required.adapterLuid.HighPart &&
        description.AdapterLuid.LowPart == required.adapterLuid.LowPart && device->GetFeatureLevel() >= required.minFeatureLevel;
    if (!compatible) { Msg("! [VR] engine D3D11 device does not meet runtime requirements"); Shutdown(); return false; }
    m_device = device; device->GetImmediateContext(&m_context);
    auto binding = vr::MakeD3D11Binding(device);
    XrSessionCreateInfo info{XR_TYPE_SESSION_CREATE_INFO};
    info.next = &binding; info.systemId = m_system;
    bool ok = vr::Succeeded(xrCreateSession(m_instance, &info, &m_session), "xrCreateSession") &&
        CreateSpaces() && vr::CreateEyeSwapchains(device, m_instance, m_session, m_system, m_graphics,
            width, height, format, m_eyes);
    if (ok)
    {
        XrViewConfigurationView ui{XR_TYPE_VIEW_CONFIGURATION_VIEW};
        ui.recommendedImageRectWidth = ui.maxImageRectWidth = Width();
        ui.recommendedImageRectHeight = ui.maxImageRectHeight = Height();
        ok = vr::CreateEyeSwapchain(device, m_session, ui, m_eyes[0].format, 1, Width(), Height(), m_ui) &&
            VrInput::Get().Initialize(m_instance, m_session, m_hp_extension);
    }
    if (!ok) { Shutdown(); return false; }
    // Optional: without it the compositor reprojects colour only.
    if (m_depth_extension && !vr::CreateDepthSwapchains(m_session, Width(), Height(), m_depth))
        m_depth_extension = false;
    m_start_time = GetTickCount64();
    // Polling frames (including empty ones) lets runtimes progress to VISIBLE.
    while (!m_lost && m_state != XR_SESSION_STATE_VISIBLE && m_state != XR_SESSION_STATE_FOCUSED &&
        GetTickCount64() - m_start_time < 5000)
    {
        MSG message;
        while (PeekMessage(&message, nullptr, 0, 0, PM_REMOVE))
        {
            if (message.message == WM_QUIT) { PostQuitMessage(int(message.wParam)); m_lost = true; break; }
            TranslateMessage(&message); DispatchMessage(&message);
        }
        PollEvents();
        if (IsActive() && PrepareFrame()) SubmitFrame();
        else Sleep(1);
    }
    if (m_lost || (m_state != XR_SESSION_STATE_VISIBLE && m_state != XR_SESSION_STATE_FOCUSED))
    {
        Msg("! [VR] session unavailable after startup wait; desktop fallback"); Shutdown(); return false;
    }
    Msg("* [VR] stereo session ready: %ux%u per eye", Width(), Height());
    return true;
}
void CVrSystem::Shutdown()
{
    if (m_frame_begun) SubmitFrame();
    VrInput::Get().Shutdown();
    for (auto& eye : m_eyes) vr::DestroyEyeSwapchain(eye);
    for (auto& depth : m_depth) vr::DestroyEyeSwapchain(depth);
    m_depth_extension = m_depth_submitted[0] = m_depth_submitted[1] = false;
    vr::DestroyEyeSwapchain(m_ui);
    if (m_head_space && xrDestroySpace) xrDestroySpace(m_head_space);
    if (m_stage_space && xrDestroySpace) xrDestroySpace(m_stage_space);
    if (m_local_space && xrDestroySpace) xrDestroySpace(m_local_space);
    m_head_space = m_stage_space = m_local_space = m_view_space = XR_NULL_HANDLE;
    if (m_session && xrDestroySession) xrDestroySession(m_session);
    if (m_instance && xrDestroyInstance) xrDestroyInstance(m_instance);
    m_session = XR_NULL_HANDLE; m_instance = XR_NULL_HANDLE; m_system = XR_NULL_SYSTEM_ID;
    for (auto& sample : m_timing)
    {
        if (sample.disjoint) sample.disjoint->Release();
        if (sample.begin) sample.begin->Release();
        if (sample.end) sample.end->Release();
        sample = {};
    }
    m_timing_slot = -1;
    if (m_context) m_context->Release();
    m_context = nullptr; m_device = nullptr;
    m_session_running = m_frame_begun = m_lost = m_views_valid = false;
    m_head_valid = m_origin_valid = m_preserve_origin = m_ui_ready = m_recenter_pending = false;
    m_menu_active=m_inventory_active=m_menu_anchor_valid=false;
    m_scene_rendered = m_scene_attempted = false;
    m_body_owner = nullptr; m_gameplay_bound = false; m_world_offset.set(0,0,0);
    m_space_change_time = 0; m_state = XR_SESSION_STATE_UNKNOWN;
    g_vr_owns_present = false;
    vr::UnloadOpenXR();
}
void CVrSystem::HandleSessionStateChanged(const XrEventDataSessionStateChanged& event)
{
    if (event.session != m_session) return;
    m_state = event.state;
    Msg("* [VR] session -> %s", StateName());
    if (m_state == XR_SESSION_STATE_READY && !m_session_running)
    {
        XrSessionBeginInfo info{XR_TYPE_SESSION_BEGIN_INFO};
        info.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        m_session_running = Check(xrBeginSession(m_session, &info), "xrBeginSession");
    }
    else if (m_state == XR_SESSION_STATE_STOPPING)
    {
        // The runtime pauses the session (headset asleep, link dropped): end
        // it and wait for READY to begin it again. Only LOSS_PENDING and
        // EXITING are a lost session that needs the recovery dialog.
        if (m_frame_begun) SubmitFrame();
        if (m_session_running) Check(xrEndSession(m_session), "xrEndSession");
        m_session_running = false;
        m_views_valid = false;
        m_origin_valid = false; m_preserve_origin = m_head_valid && !m_recenter_pending;
        VrInput::Get().Invalidate();
    }
    else if (m_state == XR_SESSION_STATE_LOSS_PENDING || m_state == XR_SESSION_STATE_EXITING)
    {
        m_lost = true; m_session_running = false;
    }
}
void CVrSystem::PollEvents()
{
    if (!m_instance || m_lost) return;
    for (;;)
    {
        XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
        const XrResult result = xrPollEvent(m_instance, &event);
        if (result == XR_EVENT_UNAVAILABLE) break;
        if (!Check(result, "xrPollEvent")) break;
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
            HandleSessionStateChanged(*reinterpret_cast<XrEventDataSessionStateChanged*>(&event));
        else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) m_lost = true;
        else if (event.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING)
        {
            const auto& change = *reinterpret_cast<XrEventDataReferenceSpaceChangePending*>(&event);
            const auto type = m_view_space == m_stage_space ? XR_REFERENCE_SPACE_TYPE_STAGE : XR_REFERENCE_SPACE_TYPE_LOCAL;
            if (change.session == m_session && change.referenceSpaceType == type) m_space_change_time = change.changeTime;
        }
        if (m_lost) break;
    }
}
Fmatrix CVrSystem::PoseMatrix(const XrPosef& pose) const
{
    Fmatrix matrix;
    matrix.identity();
    vr::ToEngineBasis(pose.orientation, matrix.i, matrix.j, matrix.k);
    matrix.c = vr::ToEnginePosition(pose.position);
    return matrix;
}
bool CVrSystem::UpdateTracking()
{
    if (vr_tracking_origin != m_origin_mode || m_world_scale != vr_world_scale)
    {
        m_origin_mode = vr_tracking_origin;
        m_world_scale = vr_world_scale;
        m_view_space = m_origin_mode == 1 && m_stage_space ? m_stage_space : m_local_space;
        m_origin_valid = false; m_preserve_origin = m_head_valid && !m_recenter_pending;
        m_menu_anchor_valid=false;
    }
    if (m_space_change_time && m_frame_state.predictedDisplayTime >= m_space_change_time)
    {
        m_space_change_time = 0;
        // Runtime recenter moves the tracking space, not the actor. Re-anchor
        // the newly located head to the body rather than keeping an old lean.
        Recenter();
    }
    XrViewLocateInfo info{XR_TYPE_VIEW_LOCATE_INFO};
    info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    info.displayTime = m_frame_state.predictedDisplayTime; info.space = m_view_space;
    XrViewState state{XR_TYPE_VIEW_STATE};
    uint32_t count = 0;
    const XrResult result = xrLocateViews(m_session, &info, &state, 2, &count, m_views);
    const XrViewStateFlags valid = XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
    m_views_valid = Check(result, "xrLocateViews") && count == 2 && (state.viewStateFlags & valid) == valid;
    XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
    if (m_views_valid)
        m_views_valid = Check(xrLocateSpace(m_head_space, m_view_space, info.displayTime, &head), "locate head") &&
            (head.locationFlags & (XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) ==
            (XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
    if (!m_views_valid)
    {
        if (m_head_valid) { m_origin_valid = false; m_preserve_origin = !m_recenter_pending; }
        return false;
    }
    m_head_pose = head.pose;
    if (m_menu_active && !m_menu_anchor_valid && m_state==XR_SESSION_STATE_FOCUSED)
    { m_menu_anchor=m_head_pose; m_menu_anchor_valid=true; }
    Fmatrix tracking_head = PoseMatrix(m_head_pose);
    if (!m_origin_valid)
    {
        if (m_preserve_origin && !m_recenter_pending)
        {
            Fmatrix inverse; inverse.invert(tracking_head);
            m_tracking_to_body.mul_43(m_head_body, inverse);
        }
        else
        {
            AlignTrackingOrigin();
        }
        m_origin_valid = true; m_preserve_origin = false;
    }
    m_head_body.mul_43(m_tracking_to_body, tracking_head);
    m_head_valid = true;
    return true;
}
void CVrSystem::BeginTiming()
{
    if (!vr_debug || !m_context) return;
    QueryPerformanceCounter(&m_cpu_begin);
    auto& sample = m_timing[m_timing_cursor++ % 4];
    if (sample.pending)
    {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
        UINT64 start = 0, finish = 0;
        if (m_context->GetData(sample.disjoint, &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            m_context->GetData(sample.begin, &start, sizeof(start), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
            m_context->GetData(sample.end, &finish, sizeof(finish), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) return;
        if (!disjoint.Disjoint && disjoint.Frequency)
        {
            m_gpu_ms = 1000.0 * double(finish - start) / double(disjoint.Frequency);
            m_gpu_sum += m_gpu_ms; m_gpu_max = std::max(m_gpu_max, m_gpu_ms); ++m_gpu_count;
        }
        sample.pending = false;
    }
    if (!sample.disjoint)
    {
        D3D11_QUERY_DESC query{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        if (FAILED(m_device->CreateQuery(&query, &sample.disjoint))) return;
        query.Query = D3D11_QUERY_TIMESTAMP;
        if (FAILED(m_device->CreateQuery(&query, &sample.begin)) || FAILED(m_device->CreateQuery(&query, &sample.end)))
        {
            if (sample.begin) sample.begin->Release();
            sample.disjoint->Release(); sample = {};
            return;
        }
    }
    m_timing_slot = int((&sample) - m_timing);
    m_context->Begin(sample.disjoint); m_context->End(sample.begin);
}
void CVrSystem::EndTiming()
{
    if (m_timing_slot >= 0)
    {
        auto& sample = m_timing[m_timing_slot];
        m_context->End(sample.end); m_context->End(sample.disjoint);
        sample.pending = true; m_timing_slot = -1;
    }
    if (!vr_debug) { m_cpu_sum = m_cpu_max = m_gpu_sum = m_gpu_max = 0; m_cpu_count = m_gpu_count = 0; return; }
    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now); QueryPerformanceFrequency(&frequency);
    const double cpu = 1000.0 * double(now.QuadPart - m_cpu_begin.QuadPart) / double(frequency.QuadPart);
    m_cpu_sum += cpu; m_cpu_max = std::max(m_cpu_max, cpu); ++m_cpu_count;
    if (Device.dwFrame % 120 == 0 && m_cpu_count)
    {
        // Every frame since the last line, with the settings being compared.
        Msg("* [VR] frames=%u CPU avg=%.2f max=%.2f ms, GPU avg=%.2f max=%.2f ms (%u samples), "
            "budget=%.2f ms, shared_shadows=%d scope_quality=%.2f",
            m_cpu_count, m_cpu_sum / m_cpu_count, m_cpu_max,
            m_gpu_count ? m_gpu_sum / m_gpu_count : 0.0, m_gpu_max, m_gpu_count,
            double(m_frame_state.predictedDisplayPeriod) / 1e6, vr_shared_shadows, vr_scope_quality);
        m_cpu_sum = m_cpu_max = m_gpu_sum = m_gpu_max = 0; m_cpu_count = m_gpu_count = 0;
    }
}
bool CVrSystem::PrepareFrame()
{
    if (!IsActive() || m_frame_begun) return false;
    XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};
    const XrResult wait_result=xrWaitFrame(m_session, &wait, &m_frame_state);
    if (wait_result==XR_TIMEOUT_EXPIRED)
    {
        Msg("* [VR] xrWaitFrame timed out; skipping this simulation sample");
        m_views_valid = false;
        VrInput::Get().Invalidate();
        return false;
    }
    if (!Check(wait_result, "xrWaitFrame"))
    {
        m_views_valid = false;
        VrInput::Get().Invalidate();
        return false;
    }
    XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO};
    const XrResult begin_result = xrBeginFrame(m_session, &begin);
    if (XR_FAILED(begin_result)) { Check(begin_result, "xrBeginFrame"); return false; }
    // Qualified success (including LOSS_PENDING) still begins a frame and
    // must be paired with xrEndFrame before session recovery.
    m_frame_begun = true; m_scene_rendered = m_scene_attempted = m_ui_ready = false;
    if (!Check(begin_result, "xrBeginFrame")) { SubmitFrame(); return false; }
    BeginTiming();
    UpdateTracking();
    VrInput::Get().Sync(m_session, m_frame_state.predictedDisplayTime, m_view_space);
    if (m_frame_state.shouldRender && !m_lost)
        {
            m_ui_ready = vr::AcquireEyeImage(m_ui, "acquire UI") && vr::WaitEyeImage(m_ui, "wait UI");
            if (!m_ui_ready) m_lost = true;
            else { const float clear[] = {0,0,0,0}; m_context->ClearRenderTargetView(UiTarget(), clear); }
        }
    return true;
}
ID3D11RenderTargetView* CVrSystem::UiTarget() const
{
    return m_ui_ready ? m_ui.views[m_ui.acquired_index] : nullptr;
}
ID3D11Texture2D* CVrSystem::MirrorTexture() const
{
    // Only expose images while they are acquired and waited, never after release.
    if (m_ui_ready) return m_ui.images[m_ui.acquired_index].texture;
    return nullptr;
}
bool CVrSystem::RenderScene(VrEyeRenderFn render, void* user)
{
    if (!m_frame_begun) return false;
    if (m_scene_attempted) return true;
    m_scene_attempted = true;
    if (!m_frame_state.shouldRender || !m_views_valid || m_lost) return true;
    unsigned rendered = 0;
    // The desktop 0.2 m near plane cuts away a weapon raised to the eye and
    // the hands holding it; the eye views use the VR one (see RenderEye).
    // That close a plane costs 24-bit depth precision in the distance, so it
    // is only used while a hand (and what it holds: a stock reaches 45 cm
    // back from the grip) is near the face; otherwise up to 10 cm.
    float closest = 1e30f; // none tracked: the far plane
    for (int hand = 0; hand < 2; ++hand)
    {
        XrPosef grip;
        if (!VrInput::Get().GripPose(hand, grip)) continue;
        const float x = grip.position.x - m_head_pose.position.x, y = grip.position.y - m_head_pose.position.y,
            z = grip.position.z - m_head_pose.position.z;
        closest = std::min(closest, std::sqrt(x*x + y*y + z*z));
    }
    const float near_plane = vr::AdaptiveNearPlane(closest, vr_near_plane, m_world_scale);
    const float far_plane = g_pGamePersistent && g_pGamePersistent->Environment().CurrentEnv ?
        g_pGamePersistent->Environment().CurrentEnv->far_plane : 1000.f;
    const auto to_world = [&](const XrPosef& pose)
    {
        Fmatrix local, world;
        local.mul_43(m_tracking_to_body, PoseMatrix(pose));
        world.mul_43(m_body_world, local);
        world.c.x += m_world_offset.x; world.c.y += m_world_offset.y; world.c.z += m_world_offset.z;
        return world;
    };
    const Fmatrix eye_world[2] = {to_world(m_views[0].pose), to_world(m_views[1].pose)};
    // The shadow camera: the head's orientation, between the eyes, its
    // frustum the union of theirs (each eye's corners taken into the head's
    // frame, so canted displays are covered too).
    Fmatrix shadow_world = to_world(m_head_pose);
    shadow_world.c.add(eye_world[0].c, eye_world[1].c).mul(0.5f);
    vr::FovAngles shadow_fov;
    for (int eye = 0; eye < 2; ++eye)
    {
        const XrFovf& fov = m_views[eye].fov;
        for (int corner = 0; corner < 4; ++corner)
        {
            Fvector direction;
            direction.set(0, 0, 0)
                .mad(eye_world[eye].i, tanf(corner & 1 ? fov.angleRight : fov.angleLeft))
                .mad(eye_world[eye].j, tanf(corner & 2 ? fov.angleUp : fov.angleDown))
                .add(eye_world[eye].k);
            shadow_fov.Include(direction.dotproduct(shadow_world.i), direction.dotproduct(shadow_world.j),
                direction.dotproduct(shadow_world.k));
        }
    }
    XrFovf shadow_angles{shadow_fov.left, shadow_fov.right, shadow_fov.up, shadow_fov.down};
    if (shadow_fov.empty) shadow_angles = m_views[0].fov;
    Fmatrix shadow_view; shadow_view.invert(shadow_world);
    const Fmatrix shadow_projection = vr::MakeAsymmetricProjection(shadow_angles, near_plane, far_plane);
    m_depth_near = near_plane; m_depth_far = far_plane;
    for (int eye = 0; eye < 2; ++eye)
    {
        auto& swapchain = m_eyes[eye];
        if (!vr::AcquireEyeImage(swapchain, "acquire eye") || !vr::WaitEyeImage(swapchain, "wait eye"))
        { m_lost = true; break; }
        auto& depth = m_depth[eye];
        const bool with_depth = m_depth_extension && depth.handle;
        if (with_depth && (!vr::AcquireEyeImage(depth, "acquire depth") || !vr::WaitEyeImage(depth, "wait depth")))
        { m_lost = true; vr::ReleaseEyeImage(swapchain); break; }
        VrEyeRenderContext context;
        context.eye = eye; context.target = swapchain.views[swapchain.acquired_index];
        context.width = swapchain.width; context.height = swapchain.height;
        context.depth = with_depth ? depth.images[depth.acquired_index].texture : nullptr;
        context.view.invert(eye_world[eye]);
        context.projection = vr::MakeAsymmetricProjection(m_views[eye].fov, near_plane, far_plane);
        context.near_plane = near_plane;
        context.shadow_view = shadow_view;
        context.shadow_projection = shadow_projection;
        const bool ok = render && render(context, user);
        const bool released = vr::ReleaseEyeImage(swapchain) && (!with_depth || vr::ReleaseEyeImage(depth));
        if (!released) m_lost = true;
        m_depth_submitted[eye] = with_depth && ok && released;
        if (!ok || !released) break;
        ++rendered;
    }
    m_scene_rendered = rendered == 2;
    return true;
}
void CVrSystem::SubmitFrame()
{
    if (!m_frame_begun) return;
    XrCompositionLayerProjectionView views[2]{{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    XrCompositionLayerDepthInfoKHR depth[2]{{XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR}, {XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR}};
    for (int eye = 0; eye < 2; ++eye)
    {
        views[eye].pose = m_views[eye].pose; views[eye].fov = m_views[eye].fov;
        views[eye].subImage.swapchain = m_eyes[eye].handle;
        views[eye].subImage.imageRect.extent = {m_eyes[eye].width, m_eyes[eye].height};
        if (!m_depth_submitted[0] || !m_depth_submitted[1]) continue;
        // Standard (not reversed) depth from the eye's projection, in the
        // engine's world units; the runtime's reprojection uses it.
        depth[eye].subImage.swapchain = m_depth[eye].handle;
        depth[eye].subImage.imageRect.extent = {m_depth[eye].width, m_depth[eye].height};
        depth[eye].minDepth = 0.f; depth[eye].maxDepth = 1.f;
        depth[eye].nearZ = m_depth_near / m_world_scale; depth[eye].farZ = m_depth_far / m_world_scale;
        views[eye].next = &depth[eye];
    }
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    projection.space = m_view_space; projection.viewCount = 2; projection.views = views;
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    quad.space = m_head_space; quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad.subImage.swapchain = m_ui.handle;
    quad.subImage.imageRect.extent = {m_ui.width, m_ui.height};
    quad.pose.orientation.w = 1; quad.pose.position.z = -1.5f;
    if (m_menu_active && m_menu_anchor_valid)
    {
        // Keep the panel in tracking space, independent of subsequent head motion.
        quad.space=m_view_space; quad.pose=m_menu_anchor;
        Fvector right,up,forward;
        vr::ToEngineBasis(m_menu_anchor.orientation,right,up,forward);
        quad.pose.position.x+=forward.x*1.5f;
        quad.pose.position.y+=forward.y*1.5f;
        quad.pose.position.z-=forward.z*1.5f;
    }
    quad.size.width = m_inventory_active ? 1.92f : 1.5f;
    quad.size.height = quad.size.width * vr::kUiPanelAspect;
    const XrCompositionLayerBaseHeader* layers[2];
    uint32_t count = 0;
    if (!m_lost && m_frame_state.shouldRender)
    {
        if (m_scene_rendered) layers[count++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
        if (m_ui_ready && vr::ReleaseEyeImage(m_ui) && (!m_menu_active || m_menu_anchor_valid)) layers[count++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
    }
    // Cleanup also runs for shouldRender=false and error paths.
    for (auto& eye : m_eyes) if (eye.acquired) vr::ReleaseEyeImage(eye);
    for (auto& image : m_depth) if (image.acquired) vr::ReleaseEyeImage(image);
    m_depth_submitted[0] = m_depth_submitted[1] = false;
    if (m_ui.acquired) vr::ReleaseEyeImage(m_ui);
    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
    end.displayTime = m_frame_state.predictedDisplayTime;
    end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    end.layerCount = count; end.layers = count ? layers : nullptr;
    EndTiming();
    Check(xrEndFrame(m_session, &end), "xrEndFrame");
    m_frame_begun = false; m_ui_ready = false;
    if (vr_debug && (++m_frames % 120 == 0))
        Msg("* [VR] state=%s views=%d stereo=%d head=(%.3f %.3f %.3f)", StateName(), m_views_valid,
            m_scene_rendered, m_head_body.c.x, m_head_body.c.y, m_head_body.c.z);
}
void CVrSystem::AlignTrackingOrigin()
{
    const Fmatrix tracking_head = PoseMatrix(m_head_pose);
    Fvector forward = tracking_head.k; forward.y = 0;
    if (forward.square_magnitude() < EPS_S) forward.set(0, 0, 1);
    forward.normalize();
    Fmatrix origin; origin.identity(); origin.k = forward;
    origin.i.crossproduct(origin.j, origin.k); origin.c = tracking_head.c;
    m_tracking_to_body.invert(origin);
    m_head_body.mul_43(m_tracking_to_body, tracking_head);
    m_origin_valid = true; m_preserve_origin = false; m_recenter_pending = false;
    m_world_offset.set(0,0,0);
}
void CVrSystem::SetGameplayBody(const Fmatrix& world, const void* owner)
{
    m_world_offset.set(0,0,0);
    m_head_blocked = 0.f;
    if (owner && owner != m_body_owner && m_views_valid)
    {
        // Startup menus may have sampled an HMD resting on a desk. Calibrate
        // at the actual playable actor's eye anchor, never the floor or a
        // third-person desktop camera. Keep physical pitch/roll and eye poses.
        AlignTrackingOrigin();
        Msg("* [VR] eye-height calibration: runtime=%.3f anchor=%.3f offset=%.3f",
            m_head_pose.position.y, world.c.y, m_head_body.c.y);
    }
    else if (owner && owner == m_body_owner && m_gameplay_bound && m_views_valid)
    {
        // Yaw turns pivot around the tracked head, so leaning does not orbit
        // the camera. Body translation still follows ordinary actor physics.
        const Fvector p = m_head_body.c;
        const Fvector old_offset{
            m_body_world.i.x*p.x + m_body_world.j.x*p.y + m_body_world.k.x*p.z,
            m_body_world.i.y*p.x + m_body_world.j.y*p.y + m_body_world.k.y*p.z,
            m_body_world.i.z*p.x + m_body_world.j.z*p.y + m_body_world.k.z*p.z};
        const Fvector rotated{
            world.i.x*p.x + world.j.x*p.y + world.k.x*p.z,
            world.i.y*p.x + world.j.y*p.y + world.k.y*p.z,
            world.i.z*p.x + world.j.z*p.y + world.k.z*p.z};
        const Fvector delta{old_offset.x-rotated.x, old_offset.y-rotated.y, old_offset.z-rotated.z};
        const Fvector local{delta.x*world.i.x+delta.y*world.i.y+delta.z*world.i.z,
            delta.x*world.j.x+delta.y*world.j.y+delta.z*world.j.z,
            delta.x*world.k.x+delta.y*world.k.y+delta.z*world.k.z};
        m_tracking_to_body.c.x += local.x; m_tracking_to_body.c.y += local.y; m_tracking_to_body.c.z += local.z;
        m_head_body.c.x += local.x; m_head_body.c.y += local.y; m_head_body.c.z += local.z;
    }
    // Do not bind invalid tracking: the first valid gameplay frame must calibrate.
    if (owner && m_views_valid) m_body_owner = owner;
    m_gameplay_bound = owner && m_views_valid;
    m_body_world = world;
}
Fvector CVrSystem::WorldHeadPosition() const { return TrackingToWorld(m_head_pose).c; }
bool CVrSystem::WorldToUi(const Fvector& world, float& u, float& v) const
{
    u=v=-1.f;
    if (m_menu_active || !m_views_valid || !m_ui.width) return false;
    // SubmitFrame places the gameplay quad 1.5 m ahead of the head, 1.5 m
    // wide. Intersect the eye-to-point ray with that plane.
    Fmatrix inverse; inverse.invert(TrackingToWorld(m_head_pose));
    Fvector local=world; inverse.transform_tiny(local);
    if (local.z<0.05f) return false;
    const float width=1.5f, height=width*vr::kUiPanelAspect;
    u=0.5f+1.5f*local.x/local.z/width;
    v=0.5f-1.5f*local.y/local.z/height;
    return u>=0.f && u<=1.f && v>=0.f && v<=1.f;
}
void CVrSystem::ConstrainHead(const Fvector& position)
{
    const Fvector desired = WorldHeadPosition();
    m_world_offset.set(position.x-desired.x, position.y-desired.y, position.z-desired.z);
    m_head_blocked = std::sqrt(m_world_offset.square_magnitude());
}
bool CVrSystem::HeadWorld(Fmatrix& world) const
{
    if (!m_views_valid) return false;
    world = TrackingToWorld(m_head_pose);
    return true;
}
void CVrSystem::ShiftTrackingOrigin(const Fvector& world_delta)
{
    // Into the body's frame (its rotation is orthonormal: transpose).
    const Fmatrix& b = m_body_world;
    const Fvector local{
        world_delta.x*b.i.x + world_delta.y*b.i.y + world_delta.z*b.i.z,
        world_delta.x*b.j.x + world_delta.y*b.j.y + world_delta.z*b.j.z,
        world_delta.x*b.k.x + world_delta.y*b.k.y + world_delta.z*b.k.z};
    m_tracking_to_body.c.x -= local.x; m_tracking_to_body.c.y -= local.y; m_tracking_to_body.c.z -= local.z;
    m_head_body.c.x -= local.x; m_head_body.c.y -= local.y; m_head_body.c.z -= local.z;
}
void CVrSystem::SetMenuActive(bool active, bool inventory)
{
    const bool anchored=active || inventory;
    if (anchored!=m_menu_active || inventory!=m_inventory_active) m_menu_anchor_valid=false;
    m_menu_active=anchored;
    m_inventory_active=inventory;
    // Capture on the next fresh, focused tracking sample, not a cached pose
    // from loading or an HMD resting on the desk before it is worn.
}
bool CVrSystem::UiAim(int hand, float& u, float& v, bool* inside) const
{
    u=v=-1.f;
    if (inside) *inside=false;
    XrPosef aim;
    if ((m_menu_active && !m_menu_anchor_valid) || !HasInputFocus() || !VrInput::Get().HasRecentState() || !VrInput::Get().AimPose(hand, aim)) return false;
    Fmatrix inverse_head, local;
    inverse_head.invert(PoseMatrix(m_menu_active ? m_menu_anchor : m_head_pose));
    local.mul_43(inverse_head, PoseMatrix(aim));
    const float scale = vr_world_scale;
    // Match SubmitFrame's inventory quad. With the menu's 1.5 m width,
    // inventory rays were offset/scaled and missed most inventory cells.
    const float width = (m_inventory_active ? 1.92f : 1.5f)*scale;
    const float height = width*vr::kUiPanelAspect;
    bool on_panel=false;
    const bool tracked=vr::QuadRayCursor(local.c,local.k,1.5f*scale,
        width,height,u,v,on_panel);
    if (inside) *inside=on_panel;
    return tracked;
}
Fmatrix CVrSystem::TrackingToWorld(const XrPosef& pose) const
{
    Fmatrix local, world;
    local.mul_43(m_tracking_to_body, PoseMatrix(pose));
    world.mul_43(m_body_world, local);
    world.c.x += m_world_offset.x; world.c.y += m_world_offset.y; world.c.z += m_world_offset.z;
    return world;
}
void CVrSystem::Recenter()
{
    // Keep the request pending across unavailable/invalid tracking samples.
    m_recenter_pending=true; m_origin_valid=false; m_preserve_origin=false;
    m_menu_anchor_valid=false;
    m_world_offset.set(0,0,0);
}
bool CVrSystem::HeadPose(Fvector& position, Fvector& direction, Fvector& up) const
{
    if (!m_views_valid) return false;
    position = m_head_body.c; direction = m_head_body.k; up = m_head_body.j;
    return true;
}
#endif
