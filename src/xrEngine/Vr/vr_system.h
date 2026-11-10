#pragma once
#include "vr_common.h"
#include "vr_graphics_d3d11.h"
#include "vr_body_tracking.h"
#include "vr_visibility_mask.h"
#include <vector>
#ifdef VR_ENABLED

struct VrEyeRenderContext
{
    int eye;
    ID3D11RenderTargetView* target;
    int width, height;
    // The runtime's depth image for this eye (ID3D11Texture2D, same size,
    // D24S8 family) to copy the eye's depth into, or null when the runtime
    // takes no depth (XR_KHR_composition_layer_depth).
    void* depth;
    Fmatrix view, projection;
    float near_plane; // this frame's eye near plane (engine units)
    // One camera between the eyes whose frustum takes in both: the sun's
    // shadow cascades are fitted to it once and shared by the two eyes.
    Fmatrix shadow_view, shadow_projection;
};
using VrEyeRenderFn = bool (*)(const VrEyeRenderContext&, void*);

class CVrSystem
{
public:
    static CVrSystem& Get();
    bool Prepare(); // instance/system/graphics requirements, before D3D creation
    bool Initialize(ID3D11Device*, unsigned, unsigned, int);
    void Shutdown();
    void PollEvents();
    bool PrepareFrame();
    bool RenderScene(VrEyeRenderFn, void*);
    void SubmitFrame();
    bool IsActive() const { return m_session_running && !m_lost; }
    bool IsRendering() const { return m_frame_begun; }
    bool NeedsRecovery() const { return m_lost; }
    bool HasSession() const { return m_session != XR_NULL_HANDLE; }
    bool HasInputFocus() const { return IsActive() && m_state == XR_SESSION_STATE_FOCUSED && m_views_valid; }
    bool UiAim(int hand, float& u, float& v, bool* inside = nullptr) const;
    // The UI quad's width and distance from the eyes (m): menus and the
    // inventory at vr_ui_distance; the in-game HUD at vr_hud_distance,
    // vr_hud_scale times the menu's width.
    void UiPanel(float& width, float& distance) const;
    bool UiPanelAnchored() const { return m_menu_anchor_valid; }
    void SetMenuActive(bool active, bool inventory = false);
    const vr::GraphicsConfig& Graphics() const { return m_graphics; }
    unsigned Width() const { return m_eyes[0].width; }
    unsigned Height() const { return m_eyes[0].height; }
    ID3D11RenderTargetView* UiTarget() const;
    ID3D11Texture2D* MirrorTexture() const;
    ID3D11Device* D3DDevice() const { return m_device; }
    XrInstance Instance() const { return m_instance; }
    XrSession Session() const { return m_session; }
    // Head position, forward and up in the body's yaw-only frame (not world).
    bool HeadPose(Fvector&, Fvector&, Fvector&) const;
    // The head's frame in world space: i right, j up, k forward, c position.
    bool HeadWorld(Fmatrix& world) const;
    const Fmatrix& BodyWorld() const { return m_body_world; }
    // Roomscale: the body anchor is about to move by world_delta under a
    // head that stays put. Moves the tracking origin back by as much.
    void ShiftTrackingOrigin(const Fvector& world_delta);
    // How far (m) head collision held the head short of where it is.
    float HeadBlocked() const { return m_head_blocked; }
    void SetWorldOrigin(const Fmatrix& view) { m_body_world.invert(view); }
    void SetGameplayBody(const Fmatrix& world, const void* owner);
    Fvector WorldHeadPosition() const;
    void ConstrainHead(const Fvector& position);
    bool GameplayReady() const { return m_gameplay_bound && m_state == XR_SESSION_STATE_FOCUSED && m_views_valid && !m_lost; }
    void Recenter();
    // The calibrated eye height is taken again at the head's current height
    // (position and heading unchanged).
    void RecalibrateHeight();
    // Counts the times the view was aligned onto the body (calibration and
    // every recenter): HeightRecalibration settles the eye height after each.
    unsigned AlignCount() const { return m_align_count; }
    // The body's owner is going away: the next one is calibrated afresh even
    // if it is allocated at the same address.
    void ForgetBody(const void* owner) { if (m_body_owner == owner) { m_body_owner = nullptr; m_gameplay_bound = false; } }
    Fmatrix TrackingToWorld(const XrPosef& pose) const;
    // A tracking pose in the body's frame, and the body's placement in the
    // world (with the head-collision offset): TrackingToWorld is their product.
    Fmatrix TrackingToBody(const XrPosef& pose) const;
    Fmatrix BodyPlacement() const;
    // Meta body tracking: a tracked joint (XrBodyJointFB) this frame, in the
    // same tracking space as the head and controllers (TrackingToWorld).
    bool BodyJoint(int joint, XrPosef& pose) const { return m_views_valid && m_body.Joint(joint, pose); }
    bool BodyTrackingTracked() const { return m_views_valid && m_body.Tracked(); }
    // The fidelity the runtime reports tracking at (XrBodyTrackingFidelityMETA), 0 unknown.
    int BodyTrackingFidelity() const { return m_body.Fidelity(); }
    float BodyTrackingConfidence() const { return m_body.Confidence(); }
    // The real floor (the room's STAGE floor) in the body's frame: its height
    // relative to the calibrated eye, in engine units. False without STAGE.
    bool RealFloorInBody(float& y) const;
    const char* StateName() const;
    // XR_KHR_visibility_mask: the share (0..1) of the eye's image the lenses
    // hide, once its mask and field of view are known; -1 before (or when the
    // runtime does not offer the mask).
    float VisibilityMaskShare(int eye) const { return eye >= 0 && eye < 2 ? m_mask_share[eye] : -1.f; }
private:
#ifdef XR_VR_TEST
    friend struct VrTestAccess;
#endif
    CVrSystem();
    bool CreateInstance();
    bool CreateSpaces();
    bool Check(XrResult, const char*);
    void HandleSessionStateChanged(const XrEventDataSessionStateChanged&);
    bool UpdateTracking();
    void BeginTiming();
    void EndTiming();
    struct GpuTiming
    {
        ID3D11Query* disjoint = nullptr;
        ID3D11Query* begin = nullptr;
        ID3D11Query* end = nullptr;
        bool pending = false;
    } m_timing[4];
    int m_timing_slot = -1;
    unsigned m_timing_cursor = 0;
    LARGE_INTEGER m_cpu_begin{};
    double m_gpu_ms = 0;
    // vr_debug: every frame's times since the last log line (average, worst).
    double m_cpu_sum = 0, m_cpu_max = 0, m_gpu_sum = 0, m_gpu_max = 0;
    unsigned m_cpu_count = 0, m_gpu_count = 0;

    Fmatrix PoseMatrix(const XrPosef&) const;
    XrInstance m_instance = XR_NULL_HANDLE;
    XrSystemId m_system = XR_NULL_SYSTEM_ID;
    XrSession m_session = XR_NULL_HANDLE;
    XrSpace m_local_space = XR_NULL_HANDLE, m_stage_space = XR_NULL_HANDLE;
    XrSpace m_view_space = XR_NULL_HANDLE, m_head_space = XR_NULL_HANDLE;
    vr::GraphicsConfig m_graphics;
    ID3D11Device* m_device = nullptr;
    ID3D11DeviceContext* m_context = nullptr;
    vr::EyeSwapchain m_eyes[2], m_ui;
    // Eye depth for the compositor's reprojection, when the runtime offers
    // the depth layer extension and a D24S8 depth swapchain.
    vr::EyeSwapchain m_depth[2];
    bool m_depth_extension = false, m_depth_submitted[2]{};
    float m_depth_near = 0.03f, m_depth_far = 1000.f;
    bool m_hp_extension = false;
    // XR_FB_body_tracking (and its high-fidelity request), with the tracker
    // made while vr_body_tracking is on.
    bool m_body_extension = false, m_fidelity_extension = false;
    CVrBodyTracker m_body;
    // XR_KHR_visibility_mask: each eye's hidden triangles (view-space
    // tangents), fetched at start and again when the runtime says they
    // changed. The share is measured (and logged) once the eye's field of
    // view is known.
    bool m_visibility_mask_extension = false;
    std::vector<vr::MaskPoint> m_mask_vertices[2];
    std::vector<uint32_t> m_mask_indices[2];
    bool m_mask_report[2]{};
    float m_mask_share[2]{-1.f, -1.f};
    void QueryVisibilityMask(int eye);
    void ReportVisibilityMask();
    void UpdateBodyTracking(XrTime time);
    // The STAGE space's origin (the real floor) in the tracking space.
    XrPosef m_floor_pose{{0,0,0,1},{0,0,0}};
    bool m_floor_valid = false;
    XrSessionState m_state = XR_SESSION_STATE_UNKNOWN;
    XrFrameState m_frame_state{XR_TYPE_FRAME_STATE};
    XrView m_views[2]{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    XrPosef m_head_pose{{0,0,0,1},{0,0,0}};
    XrPosef m_menu_anchor{{0,0,0,1},{0,0,0}};
    bool m_menu_active=false, m_inventory_active=false, m_menu_anchor_valid=false;
    Fmatrix m_body_world, m_tracking_to_body, m_head_body;
    const void* m_body_owner = nullptr;
    bool m_gameplay_bound = false;
    Fvector m_world_offset{0,0,0};
    float m_head_blocked = 0.f;
    void AlignTrackingOrigin();
    bool m_session_running = false, m_frame_begun = false, m_lost = false;
    bool m_views_valid = false, m_origin_valid = false, m_head_valid = false;
    bool m_recenter_pending = false;
    unsigned m_align_count = 0;
    bool m_preserve_origin = false, m_scene_rendered = false, m_scene_attempted = false;
    bool m_ui_ready = false;
    XrTime m_space_change_time = 0;
    int m_origin_mode = -1;
    float m_world_scale = 1;
    uint64_t m_start_time = 0;
    uint32_t m_frames = 0;
};
#endif
