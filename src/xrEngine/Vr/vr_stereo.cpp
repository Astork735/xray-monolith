#include "../stdafx.h"
#include "vr_gameplay.h"
#ifdef VR_ENABLED
#include "vr_gameplay_policy.h"
#include "vr_body_tracking_policy.h"
#include "vr_hand_render.h"
#include "vr_ui_policy.h"
#include "vr_stereo.h"
#include "vr_system.h"
#include "vr_performance.h"
#include "vr_config.h"
#include "vr_input.h"
#include "vr_overlay_draw.h"
#include "../render.h"
#include "../igame_persistent.h"
#include "../xr_ioconsole.h"
#include "../IGame_Level.h"
#include "../../Include/xrRender/UIRender.h"
#include <commctrl.h>
#include <tlhelp32.h>
#include <imgui.h>


namespace
{
    bool disabled_for_run = false;
    bool recovering = false;
    bool simulation_frame_attempted = false;
    unsigned gameplay_input_epoch=1;
    unsigned body_epoch=1;
    // What the last placement put where: a new epoch only when it changed
    // (or on a new frame, with a new tracking sample), so a second placement
    // in the same frame does not make readers solve again.
    u32 body_epoch_frame=u32(-1);
    Fmatrix body_epoch_body=Fidentity;
    Fvector body_epoch_head{0,0,0};
    bool gameplay_input_active=false;
    vr::TurnState turn_state;
    bool stance_armed=false;
    vr::UiClickState menu_click;
    vr::UiConfirmState loading_confirm;
    vr::UiConsumedControls loading_consumed;
    int menu_hand = -1;
    bool pointer_level_ui = false;
    bool menu_button_down = false;
    // A quick save asked for by the head gesture (vr::RequestQuickSave),
    // dispatched from PollVrEvents, outside the XR frame.
    bool quick_save_pending = false;
    // The torso's yaw in the body's frame (see vr::TorsoForward).
    float torso_yaw = 0.f;
    bool torso_valid = false;
    u32 torso_frame = u32(-1);
    vr::UiToggleState inventory_toggle;
    vr::HeightRecalibration height_recalibration;
    unsigned height_aligned_seen = 0; // CVrSystem::AlignCount last settled after
    // Comfort vignette: the latest stick motion (0..1), a snap turn, and the
    // eased level drawn this frame.
    float comfort_motion = 0.f;
    u32 comfort_motion_time = 0, comfort_snap_time = 0;
    bool comfort_snapped = false;
    float vignette_level = 0.f;
    u32 vignette_frame = u32(-1);
    // Movement stick (left, or right when left-handed); the other one turns.
    int MoveStick() { return vr_left_handed ? 1 : 0; }
    int TurnStick() { return vr_left_handed ? 0 : 1; }
    unsigned desktop_width = 0, desktop_height = 0;
    int desktop_format = 0;
    // UI input is sampled right after PrepareFrame, from the same synced
    // action sample as the pointer rays, but dispatched at the start of the
    // next frame from PollVrEvents. Menu actions may load a level, reset the
    // device, or close the game; they must not run inside a begun XR frame.
    struct UiPointerSample
    {
        bool valid=false, menu_active=false, level_ui_active=false, inventory_open=false, imgui_active=false;
        bool active=false, hit=false, down=false, right_down=false, transfer_down=false;
        int hand=-1;
        float u=-1.f, v=-1.f;
        // Each hand's own ray, for the on-screen keyboard: both hands type.
        struct Hand { bool on=false, down=false; float u=-1.f, v=-1.f; } hands[2];
    };
    UiPointerSample pointer_sample;
    vr::CursorSmoother hand_smoothers[2];
    // The menu cursor, smoothed per pointing hand (vr_cursor_smoothing).
    vr::CursorSmoother cursor_smoother;
    int cursor_hand=-1;
    u32 cursor_time=0;
    // A menu pointer is in use: the trigger and face buttons belong to it.
    bool ui_pointer_owns_buttons=false;
    vr::UiClickState menu_right_click;
    bool transfer_down=false;
    bool inventory_toggle_pending=false;
    // The debug overlay (F11) as the pointer's mouse: trigger left, B right,
    // the turning stick the wheel.
    vr::UiClickState imgui_click, imgui_right_click;
    vr::UiScrollRepeat imgui_scroll;
    u32 imgui_scroll_time=0;
    bool ImGuiPointerActive() { return Device.imgui_shown() && Device.imgui().is_input(); }
    // No pointer on any surface: the old ray is never delivered to a new one.
    void ClearSample(UiPointerSample& sample)
    {
        sample.active=sample.hit=sample.down=sample.right_down=sample.transfer_down=false;
        sample.hand=-1;
        sample.hands[0]=sample.hands[1]=UiPointerSample::Hand{};
    }
    bool MainMenuActive()
    {
        return g_pGamePersistent && g_pGamePersistent->m_pMainMenu && g_pGamePersistent->m_pMainMenu->IsActive();
    }
    bool LevelUiBlocked()
    {
        return recovering || !g_pGameLevel || MainMenuActive() ||
            (g_pGamePersistent && g_pGamePersistent->VrLoadingPrompt(false));
    }
    unsigned LoadingButtons(int hand);
    // Both hands' loading controls: the left's in bits 0-2, the right's in 3-5.
    unsigned BothLoadingButtons() { return LoadingButtons(0) | (LoadingButtons(1)<<3); }
    void SampleUiInput()
    {
        auto& system = CVrSystem::Get();
        auto& input = VrInput::Get();
        // Only a fresh, focused sample may change the latch. Losing a frame
        // while the inventory pauses simulation must not make a held X button
        // look like a new press on the following frame.
        if (system.HasInputFocus() && input.HasRecentState())
        {
            const bool x_down=input.Button(0) && !loading_consumed.Blocked(4u);
            if (inventory_toggle.Step(true,x_down) && !LevelUiBlocked())
                inventory_toggle_pending=true;
        }
        UiPointerSample sample;
        sample.valid=true;
        sample.menu_active=MainMenuActive();
        sample.level_ui_active=!sample.menu_active && g_pGameLevel && g_pGameLevel->VrUiPointerActive();
        sample.inventory_open=sample.level_ui_active && g_pGameLevel->VrInventoryOpen();
        // The debug overlay with its input on takes the pointer from the
        // menus and dialogs under it, as it takes the mouse on desktop.
        sample.imgui_active=ImGuiPointerActive();
        // Re-anchor immediately after opening/closing, and preserve that
        // world-space pose until the next toggle or recenter request.
        system.SetMenuActive(sample.menu_active || sample.imgui_active,sample.level_ui_active);
        sample.active = (sample.menu_active || sample.level_ui_active || sample.imgui_active) &&
            system.HasInputFocus() && input.HasRecentState() && !Console->bVisible;
        if (sample.active)
        {
            // Either controller can point at the panel. Prefer the left hand
            // for inventory so the right can keep holding a gun, but choose
            // the first actual panel hit rather than locking to a right pose
            // whose ray is pointing elsewhere.
            const int dominant=vr::DominantHand();
            const int preferred=sample.inventory_open ? 1-dominant : dominant;
            for (int pass=0;pass<2;++pass)
            {
                const int candidate=pass==0 ? preferred : 1-preferred;
                XrPosef pose;
                if (!input.AimPose(candidate,pose)) continue;
                float candidate_u=-1.f,candidate_v=-1.f;
                bool candidate_inside=false;
                if (!system.UiAim(candidate,candidate_u,candidate_v,&candidate_inside)) continue;
                const bool candidate_down=input.Select(candidate) || input.Trigger(candidate)>0.55f;
                if (candidate_inside)
                    sample.hands[candidate]={true,candidate_down,candidate_u,candidate_v};
                if (sample.hand<0 || (candidate_inside && !sample.hit) ||
                    (candidate_inside==sample.hit && candidate_down && !sample.down) ||
                    (candidate_inside==sample.hit && candidate==menu_hand && !sample.down))
                {
                    sample.hand=candidate; sample.hit=candidate_inside;
                    sample.u=candidate_u; sample.v=candidate_v;
                    sample.down=candidate_down;
                }
            }
        }
        // Smoothed, timed with the continual clock: menus pause the game's.
        if (sample.active && sample.hand>=0)
        {
            if (sample.hand!=cursor_hand) cursor_smoother.Reset();
            cursor_smoother.Step(sample.u,sample.v,float(Device.dwTimeContinual-cursor_time)*0.001f,
                vr_cursor_smoothing);
        }
        else cursor_smoother.Reset();
        for (int h=0;h<2;++h)
            if (sample.active && sample.hands[h].on)
                hand_smoothers[h].Step(sample.hands[h].u,sample.hands[h].v,
                    float(Device.dwTimeContinual-cursor_time)*0.001f,vr_cursor_smoothing);
            else hand_smoothers[h].Reset();
        cursor_hand=sample.active ? sample.hand : -1;
        cursor_time=Device.dwTimeContinual;
        // B is the right mouse button (item menus). The pointing hand's A
        // (right) or Y (left) double-clicks: the quick transfer between the
        // player's and a body's, box's or trader's items, or using an item.
        // On the PDA it is the right button too (the map's marker menu).
        const bool pointer_button=sample.active && sample.hand>=0 &&
            (sample.hand==1 ? input.Button(1) : input.SecondaryButton(0));
        const bool pda=sample.level_ui_active && g_pGameLevel->VrPdaOpen();
        sample.right_down=sample.active && (input.SecondaryButton(1) || (pda && pointer_button));
        sample.transfer_down=pointer_button && !pda;
        // The trigger or button that clicked a window shut stays the
        // pointer's until it is let go: a weapon in that hand must not take
        // it for a fresh pull the next frame (trigger and button bits).
        if (ui_pointer_owns_buttons && !sample.active)
            loading_consumed.Consume(BothLoadingButtons() & 0x2Du);
        ui_pointer_owns_buttons=sample.active;
        if (vr_debug && sample.active && Device.dwFrame%120==0)
            Msg("* [VR] UI pointer: level=%d inventory=%d hand=%d hit=%d uv=(%.3f %.3f) anchored=%d",
                sample.level_ui_active,sample.inventory_open,sample.hand,sample.hit,
                sample.u,sample.v,system.UiPanelAnchored());
        pointer_sample=sample;
    }
    void DispatchImGuiPointer(const UiPointerSample& sample, bool active)
    {
        ImGuiIO& io=ImGui::GetIO();
        const bool on=active && sample.active && sample.hand>=0;
        // Presses only on the panel; a drag may end off it.
        const int left=imgui_click.Step(on,sample.down && (sample.hit || imgui_click.held));
        const int right=imgui_right_click.Step(on,sample.right_down && (sample.hit || imgui_right_click.held));
        if (on) io.AddMousePosEvent(sample.u*io.DisplaySize.x,sample.v*io.DisplaySize.y);
        if (left) io.AddMouseButtonEvent(ImGuiMouseButton_Left,left>0);
        if (right) io.AddMouseButtonEvent(ImGuiMouseButton_Right,right>0);
        float x=0.f, y=0.f;
        if (on) VrInput::Get().Thumbstick(TurnStick(),x,y);
        // Timed with the continual clock: menus pause the game's.
        const int steps=imgui_scroll.Step(on ? y : 0.f,float(Device.dwTimeContinual-imgui_scroll_time)*0.001f);
        imgui_scroll_time=Device.dwTimeContinual;
        if (steps) io.AddMouseWheelEvent(0.f,float(steps));
    }
    void DispatchUiInput()
    {
        if (inventory_toggle_pending)
        {
            inventory_toggle_pending=false;
            if (!LevelUiBlocked())
            {
                g_pGameLevel->VrToggleInventory();
                if (vr_debug) Msg("* [VR] left X: inventory open=%d",g_pGameLevel->VrInventoryOpen());
            }
        }
        if (!pointer_sample.valid) return;
        UiPointerSample sample=pointer_sample;
        pointer_sample.valid=false;
        IMainMenu* menu = g_pGamePersistent ? g_pGamePersistent->m_pMainMenu : nullptr;
        const bool menu_active=menu && menu->IsActive();
        const bool level_ui_active=!menu_active && g_pGameLevel && g_pGameLevel->VrUiPointerActive();
        // The surface can change between sampling and dispatch (for example
        // the X toggle above). Never deliver an old ray to a new surface.
        const bool imgui_active=ImGuiPointerActive();
        if (menu_active!=sample.menu_active || level_ui_active!=sample.level_ui_active ||
            imgui_active!=sample.imgui_active)
            ClearSample(sample);
        DispatchImGuiPointer(sample,imgui_active);
        // The surface under the overlay sees the pointer leave: a click
        // held there is cancelled below.
        if (imgui_active) ClearSample(sample);
        if (level_ui_active!=pointer_level_ui || sample.hand!=menu_hand)
        {
            const bool cancel_pointer=vr::CancelUiClickOnPointerChange(
                level_ui_active!=pointer_level_ui,sample.hand!=menu_hand,menu_click.held);
            const int cancel = cancel_pointer ? menu_click.Step(false,false) : 0;
            if (cancel)
            {
                if (pointer_level_ui && g_pGameLevel)
                    g_pGameLevel->VrUiPointer(-1,-1,cancel,menu_right_click.Step(false,false));
                else if (menu) menu->VrPointer(-1,-1,cancel);
            }
            pointer_level_ui=level_ui_active;
            menu_hand = sample.hand;
        }
        const int button = menu_click.Step(sample.active && sample.hit, sample.down);
        const int right = menu_right_click.Step(sample.active && sample.hit && level_ui_active, sample.right_down);
        const bool transfer = sample.transfer_down && !transfer_down && sample.active && sample.hit && level_ui_active;
        transfer_down = sample.transfer_down;
        if (sample.active || button || right)
        {
            // The cursor follows a clamped edge position while the ray is
            // just off the panel, but a release there is a cancel and must
            // use off-panel coordinates rather than click an edge control.
            const bool off_panel=sample.hand<0 || (button<0 && !sample.hit);
            const float u=off_panel ? -1.f : sample.u, v=off_panel ? -1.f : sample.v;
            const bool handled=level_ui_active ?
                g_pGameLevel->VrUiPointer(u,v,button,right,transfer && !off_panel) :
                menu && menu->VrPointer(u,v,button);
            if (!handled)
                menu_click.Step(false, false);
        }
        // Then each hand on its own, for the on-screen keyboard; a key typed
        // ticks that hand.
        if (level_ui_active || menu_active)
            for (int h=0;h<2;++h)
            {
                const auto& hand=sample.hands[h];
                const bool on=sample.active && hand.on;
                const float u=on ? hand.u : -1.f, v=on ? hand.v : -1.f;
                const bool typed=level_ui_active ? g_pGameLevel->VrUiHandPointer(h,u,v,on && hand.down) :
                    menu->VrHandPointer(h,u,v,on && hand.down);
                if (typed) VrInput::Get().ApplyHaptic(h,0.15f,15000000);
            }
    }
    unsigned LoadingButtons(int hand)
    {
        auto& input=VrInput::Get();
        return ((input.Select(hand) || input.Trigger(hand)>0.55f) ? 1u : 0u) |
            (input.Grip(hand)>0.65f ? 2u : 0u) | (input.Button(hand) ? 4u : 0u);
    }
    bool LoadingControlBlocked(int hand,unsigned control)
    {
        return hand>=0 && hand<2 && loading_consumed.Blocked(control<<(unsigned(hand)*3));
    }
    bool UpdateLoadingPrompt()
    {
        auto& system=CVrSystem::Get(); auto& input=VrInput::Get();
        const bool valid=g_pGamePersistent && system.HasInputFocus() && input.HasRecentState() &&
            !Console->bVisible &&
            g_pGamePersistent->VrLoadingPrompt(false);
        unsigned buttons=0, available=0;
        if (valid) for (int hand=0;hand<2;++hand)
        {
            if (!input.HandActive(hand)) continue;
            const unsigned offset=unsigned(hand)*3;
            available |= 7u<<offset;
            buttons |= LoadingButtons(hand)<<offset;
        }
        if (!loading_confirm.Step(valid,buttons,available)) return false;
        // Runs before FrameMove/Begin; continuation may unpause or run scripts.
        ++gameplay_input_epoch; gameplay_input_active=false;
        loading_consumed.Consume(buttons);
        return g_pGamePersistent->VrLoadingPrompt(true);
    }
    // Every Device camera/cache field changed by an eye must be restored for
    // callbacks, sound, the desktop camera and the next simulation frame.
    struct CameraState
    {
        Fmatrix view = Device.mView, project = Device.mProject, full = Device.mFullTransform;
        Fmatrix invView = Device.mInvView, invProject = Device.mInvProject, invFull = Device.mInvFullTransform;
        Fmatrix previousView = Device.mView_prev, previousProject = Device.mProject_prev;
        Fmatrix hudProject = Device.mProjectHud, hudInverse = Device.mInvProjectHud, hudFull = Device.mFullTransformHud;
        Fvector position = Device.vCameraPosition, direction = Device.vCameraDirection;
        Fvector up = Device.vCameraTop, right = Device.vCameraRight;
        float fov = Device.fFOV, aspect = Device.fASPECT, near_plane = Device.ViewportNear;
        ~CameraState()
        {
            Device.ViewportNear = near_plane;
            Device.mView = view; Device.mProject = project; Device.mFullTransform = full;
            Device.mInvView = invView; Device.mInvProject = invProject; Device.mInvFullTransform = invFull;
            Device.vCameraPosition = position; Device.vCameraDirection = direction;
            Device.vCameraTop = up; Device.vCameraRight = right;
            Device.fFOV = fov; Device.fASPECT = aspect;
            Device.mView_prev = previousView; Device.mProject_prev = previousProject;
            Device.mProjectHud = hudProject; Device.mInvProjectHud = hudInverse; Device.mFullTransformHud = hudFull;
            Device.m_pRender->SetCacheXform(Device.mView, Device.mProject);
            Device.m_pRender->SetCacheXform_prev(Device.mView_prev, Device.mProject_prev);
        }
    };
    Fmatrix scope_camera = Fidentity;
    shared_str scope_reticle;
    float scope_reticle_rect[4] = {};
    bool scope_reticle_part = false;
    float scope_fov = 0.f, scope_lens = 0.f;
    u32 scope_request_frame = 0, scope_ready_frame = 0;
    // The eyes' vertical field of view (degrees), for the scope view's size.
    float eye_fov = 0.f;
    unsigned scope_size = 0;
    // The magnified scope's view, rendered like an eye into its own target
    // every other frame while the game asks for it, only as large as the
    // lens shows it (vr_scope_quality): the renderer's screen-sized targets
    // are swapped for smaller ones meanwhile.
    // The device's camera for one view (an eye, the scope): its matrices and
    // their inverses, the camera vectors, the near plane (shaders and
    // visibility read the one of the projection in use), FOV and aspect,
    // and the renderer's cached transforms.
    void SetDeviceCamera(const Fmatrix& view, const Fmatrix& project, float near_plane, float fov, float aspect)
    {
        Device.mView = view; Device.mProject = project;
        Device.ViewportNear = near_plane;
        Device.mFullTransform.mul(project, view);
        Device.mProjectHud = project; Device.mInvProjectHud.invert(project);
        Device.mFullTransformHud.mul(project, view);
        Device.mInvView.invert(view); Device.mInvProject.invert(project);
        Device.mInvFullTransform.invert(Device.mFullTransform);
        Device.vCameraPosition = Device.mInvView.c; Device.vCameraDirection = Device.mInvView.k;
        Device.vCameraTop = Device.mInvView.j; Device.vCameraRight = Device.mInvView.i;
        Device.fFOV = fov; Device.fASPECT = aspect;
        Device.mView_prev = view; Device.mProject_prev = project;
        Device.m_pRender->SetCacheXform(Device.mView, Device.mProject);
        Device.m_pRender->SetCacheXform_prev(Device.mView_prev, Device.mProject_prev);
    }
    void RenderScopeView()
    {
        if (scope_fov <= 0.f || Device.dwFrame - scope_request_frame > 1 || (Device.dwFrame & 1)) return;
        CameraState restore;
        vr::PerfViewScope perf_view(2);
        vr::PerfScope timing(vr::PerfStage::Scope, true);
        scope_size = vr::ScopeResolution(scope_lens, eye_fov, Device.dwHeight, vr_scope_quality, scope_size);
        const bool reduced = scope_size < _min(Device.dwWidth, Device.dwHeight) &&
            vr_scope_resize_begin(scope_size);
        struct ResizeEnd { bool active; ~ResizeEnd() { if (active) vr_scope_resize_end(); } } resize_end{reduced};
        const unsigned width = Device.dwWidth, height = Device.dwHeight;
        if (!vr_scope_target(width, height)) return;
        const float near_plane = 0.05f;
        const float far_plane = g_pGamePersistent && g_pGamePersistent->Environment().CurrentEnv ?
            g_pGamePersistent->Environment().CurrentEnv->far_plane : 1000.f;
        Fmatrix view; view.build_camera_dir(scope_camera.c, scope_camera.k, scope_camera.j);
        Fmatrix project; project.build_projection(deg2rad(scope_fov), float(height) / float(width), near_plane, far_plane);
        SetDeviceCamera(view, project, near_plane, scope_fov, float(height) / float(width));
        g_vr_ui_pass = false;
        g_vr_eye_index = -1;
        g_vr_rendering_eye = true;
        g_vr_eye_rendered = false;
        ::Render->Calculate();
        ::Render->Render();
        g_vr_rendering_eye = false;
        static bool reported = false, rendered = false;
        static unsigned reported_width = 0;
        if (!reported || rendered != g_vr_eye_rendered || reported_width != width)
            Msg("%s [VR] scope view %ux%u, %.1f degrees: %s", g_vr_eye_rendered ? "*" : "!", width, height,
                scope_fov, g_vr_eye_rendered ? "rendered" : "the renderer skipped it");
        reported = true; rendered = g_vr_eye_rendered; reported_width = width;
        if (g_vr_eye_rendered) scope_ready_frame = Device.dwFrame;
    }
    void UpdateVignette()
    {
        if (vignette_frame == Device.dwFrame) return;
        vignette_frame = Device.dwFrame;
        float target = vr::GameplayActive() && Device.dwTimeGlobal - comfort_motion_time < 250 ? comfort_motion : 0.f;
        if (comfort_snapped && Device.dwTimeGlobal - comfort_snap_time < 200) target = 1.f;
        else comfort_snapped = false;
        // Close quickly when motion starts, open more slowly afterwards.
        const float dt = clampr(Device.fTimeDelta, 0.f, 0.1f);
        const float rate = target > vignette_level ? 8.f : 3.f;
        vignette_level += clampr(target - vignette_level, -rate*dt, rate*dt);
    }
    void BindUi()
    {
        auto& system = CVrSystem::Get();
        if (system.UiTarget()) vr_render_target(system.UiTarget(), system.Width(), system.Height());
        else vr_render_target(nullptr, 0, 0);
        g_vr_ui_pass = system.UiTarget() != nullptr;
    }
    bool RenderEye(const VrEyeRenderContext& eye, void*)
    {
        vr::PerfViewScope perf_view(unsigned(eye.eye));
        vr::PerfScope timing(eye.eye == 0 ? vr::PerfStage::LeftEye : vr::PerfStage::RightEye, true);
        CameraState restore;
        g_vr_ui_pass = false;
        if (!vr_render_target(eye.target, eye.width, eye.height)) return false;
        g_vr_eye_index = eye.eye;
        // Auxiliary shadow/LOD calculations still accept a symmetric FOV.
        // Give them a conservative envelope; scene rasterization uses the
        // unmodified asymmetric projection.
        const float vertical = (1 + _abs(eye.projection._32)) / eye.projection._22;
        const float horizontal = (1 + _abs(eye.projection._31)) / eye.projection._11;
        SetDeviceCamera(eye.view, eye.projection, eye.near_plane, rad2deg(2 * atanf(vertical)), vertical / horizontal);
        eye_fov = Device.fFOV;
        g_vr_rendering_eye = true;
        g_vr_eye_rendered = false;
        // The sun's cascades are fitted to one camera taking in both eyes and
        // kept by the first eye; the second reuses them (vr_shared_shadows).
        g_vr_shadow_share = vr_shared_shadows ? (eye.eye == 0 ? 1 : 2) : 0;
        g_vr_shadow_view = eye.shadow_view; g_vr_shadow_project = eye.shadow_projection;
        ::Render->Calculate();
        ::Render->Render();
        g_vr_shadow_share = 0;
        if (g_vr_eye_rendered && g_pGameLevel)
        {
            vr_world_overlay();
            g_pGameLevel->OnRenderVrExtras();
        }
        if (vr_debug)
        {
            vr_world_overlay();
            const vr::OverlayDraw draw;
            auto& input = VrInput::Get();
            for (int hand = 0; hand < 2; ++hand)
                for (int aim = 0; aim < 2; ++aim)
                {
                    XrPosef pose;
                    if (!(aim ? input.AimPose(hand, pose) : input.GripPose(hand, pose))) continue;
                    const Fmatrix world = CVrSystem::Get().TrackingToWorld(pose);
                    Fvector end;
                    end.mad(world.c, world.i, 0.12f); draw.DrawLine(world.c, end, 0xffff0000);
                    end.mad(world.c, world.j, 0.12f); draw.DrawLine(world.c, end, 0xff00ff00);
                    end.mad(world.c, world.k, aim ? 0.3f : 0.12f); draw.DrawLine(world.c, end, 0xff0080ff);
                }
        }
        if (vr_hands && g_vr_eye_rendered && vr::GameplayActive() &&
            !(g_pGameLevel && g_pGameLevel->VrOutfitHands()))
        {
            for (int hand = 0; hand < 2; ++hand)
            {
                Fmatrix grip;
                if (!vr::HandTransform(hand, false, grip) || vr::HandHiddenForFrame(hand)) continue;
                const u32 color = hand ? 0xff706057 : 0xff536b75;
                vr_world_overlay();
                vr::OverlayDraw draw;
                vr::DrawHandProxy(draw, grip, hand, vr::HandSqueeze(hand), color);
            }
        }
        UpdateVignette();
        // A head pushed into a wall fades to black rather than seeing the
        // view stop while the head moves on (head collision, VrConstrainHead).
        const float blocked = vr::GameplayActive() ? vr::HeadBlockedFade(CVrSystem::Get().HeadBlocked()) : 0.f;
        const float vignette = vr_vignette > 0.f ? vignette_level*vr_vignette : 0.f;
        if (g_vr_eye_rendered && (vignette > 0.01f || blocked > 0.01f))
        {
            const float params[6] = {eye.projection._11, eye.projection._22, eye.projection._31,
                eye.projection._32, vignette > 0.01f ? vignette : 0.f, blocked};
            vr_comfort_vignette(params);
        }
        // The eye's depth for the compositor's reprojection, overlays included.
        if (eye.depth && g_vr_eye_rendered)
        {
            vr::PerfScope timing(vr::PerfStage::DepthCopy, true);
            vr_copy_eye_depth(eye.depth);
        }
        g_vr_rendering_eye = false;
        g_vr_eye_index = -1;
        // The monitor mirrors the right eye, using its texture while it is
        // still acquired.
        if (eye.eye == 1)
        {
            vr::PerfScope timing(vr::PerfStage::EyeMirror, true);
            vr_mirror(eye.target, false);
        }
        const bool ok = g_vr_eye_rendered && SUCCEEDED(CVrSystem::Get().D3DDevice()->GetDeviceRemovedReason());
        BindUi();
        return ok;
    }
}
namespace vr
{
    bool StereoActive() { return CVrSystem::Get().IsActive(); }
    bool ShouldEnterVr()
    {
        return !g_dedicated_server && !disabled_for_run && g_vr_launch_mode != VR_Launch_Off &&
            (g_vr_launch_mode == VR_Launch_Force || vr_enabled > 0);
    }
    // The active OpenXR runtime's manifest: XR_RUNTIME_JSON, else the
    // registry entry the Khronos loader reads.
    bool ActiveRuntimeManifest(char* path, DWORD size)
    {
        const DWORD env = GetEnvironmentVariableA("XR_RUNTIME_JSON", path, size);
        if (env && env < size) return true;
        HKEY key = nullptr;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Khronos\\OpenXR\\1", 0, KEY_READ, &key) != ERROR_SUCCESS)
            return false;
        DWORD type = 0, bytes = size;
        const bool ok = RegQueryValueExA(key, "ActiveRuntime", nullptr, &type, reinterpret_cast<BYTE*>(path), &bytes) ==
            ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ) && bytes > 0;
        RegCloseKey(key);
        if (ok) path[std::min<DWORD>(bytes, size - 1)] = 0;
        return ok;
    }
    bool ProcessRunning(const wchar_t* name)
    {
        const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) return false;
        PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
        bool found = false;
        for (BOOL more = Process32FirstW(snapshot, &entry); more && !found; more = Process32NextW(snapshot, &entry))
            found = _wcsicmp(entry.szExeFile, name) == 0;
        CloseHandle(snapshot);
        return found;
    }
    // Automatic mode (vr_enabled 1, no -vr) must not start a runtime the
    // player is not using: creating an OpenXR instance with SteamVR active
    // launches SteamVR even with no headset in use. Then the desktop game
    // starts on the desktop unless SteamVR is already running. -vr and
    // vr_enabled 2 still ask the runtime.
    bool AutomaticProbeAllowed()
    {
        if (g_vr_launch_mode == VR_Launch_Force || vr_enabled != 1) return true;
        char manifest[MAX_PATH * 2] = {};
        if (!ActiveRuntimeManifest(manifest, sizeof(manifest))) return true;
        _strlwr_s(manifest);
        if (!strstr(manifest, "steamxr")) return true;
        if (ProcessRunning(L"vrserver.exe")) return true;
        Msg("* [VR] automatic mode: SteamVR is the active OpenXR runtime but is not running; "
            "starting on the desktop (start SteamVR first, or launch with -vr)");
        return false;
    }
    bool PrepareVr(unsigned long& low, long& high, unsigned& feature)
    {
        if (ShouldEnterVr() && !AutomaticProbeAllowed()) { disabled_for_run = true; return false; }
        if (!ShouldEnterVr()) return false;
        if (!CVrSystem::Get().Prepare()) { disabled_for_run = true; return false; }
        const auto& requirements = CVrSystem::Get().Graphics().requirements;
        low = requirements.adapterLuid.LowPart; high = requirements.adapterLuid.HighPart;
        feature = requirements.minFeatureLevel;
        return true;
    }
    bool StartVr(ID3D11Device* device, unsigned width, unsigned height, int format)
    {
        if (!ShouldEnterVr()) return false;
        desktop_width = width; desktop_height = height; desktop_format = format;
        if (CVrSystem::Get().Initialize(device, width, height, format))
        {
            g_vr_owns_present = true;
            return true;
        }
        Msg("! [VR] unavailable; continuing on desktop");
        disabled_for_run = true;
        return false;
    }
    void StopVr()
    {
        ++gameplay_input_epoch; gameplay_input_active=false;
        turn_state.Reset();
        menu_click = {}; loading_confirm={}; loading_consumed={}; menu_hand = -1;
        pointer_level_ui=false;
        menu_button_down = false; quick_save_pending = false;
        inventory_toggle={};
        torso_valid = false; g_vr_ui_pass = false; g_vr_eye_index = -1;
        height_recalibration.Reset();
        pointer_sample={}; inventory_toggle_pending=false;
        // Let go of the overlay's buttons and give it the window again.
        if (ImGui::GetCurrentContext())
        {
            DispatchImGuiPointer({},false);
            Device.imgui().OnScreenResolutionChanged();
        }
        vr_render_target(nullptr, 0, 0); // unbind runtime-owned images before destroying them
        CVrSystem::Get().Shutdown();
        vr_renderer_release();
        g_vr_rendering_eye = false;
    }
    void PollVrEvents()
    {
        auto& system = CVrSystem::Get();
        if (!recovering && system.D3DDevice() && FAILED(system.D3DDevice()->GetDeviceRemovedReason()))
        {
            recovering = true;
            Device.Pause(TRUE, TRUE, TRUE, "VR graphics device removed");
            Msg("! [VR] D3D11 device removed: %08x; shutting down VR safely", system.D3DDevice()->GetDeviceRemovedReason());
            StopVr();
            MessageBoxW(Device.m_hWnd, L"The graphics device was removed. The game must close; restart it after the graphics driver recovers.",
                L"Anomaly VR", MB_OK | MB_ICONERROR);
            PostMessage(Device.m_hWnd, WM_CLOSE, 0, 0);
            return;
        }
        system.PollEvents();
		// Quest Touch left menu click is separate from the system-reserved
		// right Oculus button. Handle it before game simulation: toggling the
		// menu can pause or reset rendering resources.
		const bool menu_down=system.HasInputFocus() && VrInput::Get().HasRecentState() &&
			VrInput::Get().MenuButton();
		const bool menu_allowed=!recovering && !(g_pGamePersistent && g_pGamePersistent->VrLoadingPrompt(false));
		if (menu_down && !menu_button_down && menu_allowed)
			Console->Execute("main_menu");
		menu_button_down=menu_down;
		// A free hand at the head holding X or A: quick save, as the quick
		// save key (outside the XR frame).
		const bool in_menu=MainMenuActive();
		if (quick_save_pending)
		{
			quick_save_pending=false;
			if (menu_allowed && !in_menu && g_pGameLevel) g_pGameLevel->VrQuickSave();
		}
        loading_consumed.Filter(BothLoadingButtons());
        const bool input_active=vr::GameplayActive();
        if (gameplay_input_active && !input_active) ++gameplay_input_epoch;
        gameplay_input_active=input_active;
        if (!system.NeedsRecovery() || recovering)
        {
            // Dispatch the previous frame's synced UI sample before FrameMove
            // or PrepareFrame: menu actions may load a level, reset the
            // device, or close the game safely here.
            if (!recovering && !UpdateLoadingPrompt()) DispatchUiInput();
            else { pointer_sample.valid=false; inventory_toggle_pending=false; }
            return;
        }
        recovering = true;
        const bool paused = Device.Paused();
        Device.Pause(TRUE, TRUE, TRUE, "VR recovery");
        // Native dialog is available even when headset presentation is lost.
        const TASKDIALOG_BUTTON buttons[] = {{100, L"Retry VR"}, {101, L"Continue on desktop"}, {102, L"Exit"}};
        TASKDIALOGCONFIG dialog{}; dialog.cbSize = sizeof(dialog); dialog.hwndParent = Device.m_hWnd;
        dialog.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
        dialog.pszWindowTitle = L"Anomaly VR"; dialog.pszMainInstruction = L"The VR session is unavailable";
        dialog.pszContent = L"Gameplay is paused. Reconnect the headset and retry, continue on desktop, or exit.";
        dialog.cButtons = 3; dialog.pButtons = buttons; dialog.nDefaultButton = 101;
        int button = 101;
        const HMODULE controls = LoadLibraryExW(L"comctl32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        using DialogFn = HRESULT (WINAPI*)(const TASKDIALOGCONFIG*, int*, int*, BOOL*);
        const auto show = controls ? reinterpret_cast<DialogFn>(GetProcAddress(controls, "TaskDialogIndirect")) : nullptr;
        const HRESULT dialog_result = show ? show(&dialog, &button, nullptr, nullptr) : E_NOTIMPL;
        if (controls) FreeLibrary(controls);
        if (FAILED(dialog_result))
        {
            const int answer = MessageBoxW(Device.m_hWnd, L"VR disconnected. Retry VR? Select No for desktop or Cancel to exit.",
                L"Anomaly VR", MB_YESNOCANCEL | MB_ICONWARNING);
            button = answer == IDYES ? 100 : answer == IDNO ? 101 : 102;
        }
        StopVr();
        disabled_for_run = button != 100;
        if (button == 102 || button == IDCANCEL)
        {
            PostMessage(Device.m_hWnd, WM_CLOSE, 0, 0);
            recovering = false;
            return;
        }
        Device.Reset(false);
        if (!paused) Device.Pause(FALSE, TRUE, TRUE, "VR recovery");
        recovering = false;
    }
    void PrepareStereoSimulation()
    {
        // Begin and sync the predicted XR frame before gameplay consumes hand
        // poses. The render pass later reuses this same frame and action sample.
        simulation_frame_attempted = true;
        auto& system=CVrSystem::Get();
        const bool prepared=system.PrepareFrame();
        // The runtime paused the session (headset asleep): no xrWaitFrame
        // paces the loop until it is ready again, so do not spin.
        if (!prepared && system.HasSession() && !system.IsActive() && !system.NeedsRecovery()) Sleep(10);
        // Stood up after calibrating seated, or straightened up after a
        // recenter or calibration: take the eye height again.
        if (system.AlignCount() != height_aligned_seen)
        {
            height_aligned_seen = system.AlignCount();
            height_recalibration.Aligned();
        }
        // Body tracking once a frame (its easing, measuring and diagnostics),
        // whichever game code reads it.
        if (prepared) vr::UpdateBodyTracking();
        float above = 0.f;
        const bool height_valid = prepared && vr::HeadHeightDelta(above);
        const bool settling = height_recalibration.Settling();
        if (height_recalibration.Step(height_valid, above, Device.fTimeDelta))
        {
            system.RecalibrateHeight();
            if (!settling) Msg("* [VR] head %.2f m above the calibrated eye for 2 s: eye height taken again", above);
        }
        // Inventory is a level UI dialog. Use this frame's synchronized X
        // state so an open dialog can receive a second press to close.
        loading_consumed.Filter(BothLoadingButtons());
        if (prepared) SampleUiInput();
        // The debug overlay draws on the UI panel, not the window: its next
        // frame lays out on the game's 1024x768 UI grid, which reads at the
        // panel's distance (dxImGuiRender scales it to the panel texture).
        // A frame not prepared (the headset idle) draws it on the window.
        if (ImGui::GetCurrentContext())
        {
            if (prepared) ImGui::GetIO().DisplaySize={1024.f,1024.f*vr::kUiPanelAspect};
            else Device.imgui().OnScreenResolutionChanged();
        }
    }
    void BeginStereoFrame()
    {
        auto& system = CVrSystem::Get();
        if (!system.IsRendering() && (simulation_frame_attempted || !system.PrepareFrame())) return;
        // PrepareFrame already cleared the acquired, waited UI image.
        if (system.UiTarget()) BindUi();
    }
    void SubmitPendingStereoFrame()
    {
        // A D3D Begin failure must not leave xrBeginFrame unmatched.
        CVrSystem::Get().SubmitFrame();
        simulation_frame_attempted = false;
    }
    // A camera that is not the player's own (the menu, death, a vehicle or
    // turret, a cutscene) keeps its position and heading, but its pitch and
    // roll would tilt the horizon under the headset's own.
    void FlattenBody(Fmatrix& body)
    {
        body.k.y = 0;
        if (body.k.square_magnitude() < EPS_S) body.k.set(0, 0, 1);
        body.k.normalize(); body.j.set(0, 1, 0); body.i.crossproduct(body.j, body.k);
    }
    // Places the tracked body in the world for this frame: from the actor's
    // own camera when it is the player's, else from the device camera.
    const void* PlaceBody()
    {
        auto& system = CVrSystem::Get();
        Fmatrix body;
        body.invert(Device.mView);
        const bool in_menu = MainMenuActive();
        const void* owner = g_pGameLevel && !in_menu ? g_pGameLevel->VrBodyCamera(body) : nullptr;
        if (!owner) FlattenBody(body);
        system.SetGameplayBody(body, owner);
        if (owner)
        {
            Fvector head = system.WorldHeadPosition();
            g_pGameLevel->VrConstrainHead(body.c, head);
            system.ConstrainHead(head);
            if (vr_debug && Device.dwFrame % 120 == 0)
                Msg("* [VR] eye height: anchor=%.3f world=%.3f delta=%.3f blocked=%.3f", body.c.y, head.y,
                    head.y-body.c.y, system.HeadBlocked());
        }
        const Fvector head = system.WorldHeadPosition();
        if (body_epoch_frame != Device.dwFrame || !body_epoch_body.i.similar(body.i) || !body_epoch_body.j.similar(body.j) ||
            !body_epoch_body.k.similar(body.k) || !body_epoch_body.c.similar(body.c) ||
            !body_epoch_head.similar(head, EPS_L))
        {
            ++body_epoch;
            body_epoch_frame = Device.dwFrame;
            body_epoch_body = body;
            body_epoch_head = head;
        }
        return owner;
    }
    bool RenderStereoScene()
    {
        auto& system = CVrSystem::Get();
        // A session paused by the runtime (no frame this time) draws nothing:
        // there is no headset image to draw, and the mirror keeps its last.
        if (!system.IsRendering()) return g_vr_owns_present && system.HasSession();
        if (g_pGamePersistent && g_pGamePersistent->OnRenderPPUI_query())
        {
            Fmatrix menu; menu.invert(Device.mView);
            FlattenBody(menu);
            system.SetGameplayBody(menu, nullptr);
            turn_state.Reset();
            ::Render->Render(); // menu-only draw into the UI quad
            return true;
        }
        // Normally already placed by the actor this frame (UpdateGameplayBody),
        // so simulation and drawing use the same body; again here for any
        // camera change since, and for frames without a playable actor.
        const void* owner = PlaceBody();
        const bool handled = system.RenderScene(&RenderEye, nullptr);
        // After the eyes, so they keep the frame's light and sector update;
        // the lens shows it from the next frame.
        if (owner) RenderScopeView();
        BindUi();
        return handled;
    }
    void FinishStereoFrame()
    {
        auto& system = CVrSystem::Get();
        if (system.UiTarget())
        {
            vr::PerfScope timing(vr::PerfStage::UiMirror, true);
            vr_mirror(system.UiTarget(), true);
        }
        vr_render_target(nullptr, 0, 0);
        g_vr_ui_pass = false;
        system.SubmitFrame();
        simulation_frame_attempted = false;
    }
    bool HandHiddenForFrame(int hand) { extern u32 hidden_hand_frame[2]; return hidden_hand_frame[hand]==Device.dwFrame; }
    u32 hidden_hand_frame[2] = {~0u,~0u};
    void HideHandForFrame(int hand) { if (hand>=0 && hand<2) hidden_hand_frame[hand]=Device.dwFrame; }
    bool HeadDirectionInBody(Fvector& direction)
    {
        Fvector position, up;
        return GameplayActive() && CVrSystem::Get().HeadPose(position,direction,up);
    }
    bool HeadTransform(Fmatrix& world)
    {
        return GameplayActive() && CVrSystem::Get().HeadWorld(world);
    }
    bool TrackedChestYaw(float& yaw);
    bool TorsoForward(Fvector& forward)
    {
        auto& system = CVrSystem::Get();
        Fvector position, direction, up;
        if (!GameplayActive() || !system.HeadPose(position,direction,up)) return false;
        // Followed in the body's frame: a stick turn turns body and torso
        // together, so only the head's own turning moves the torso there.
        if (torso_frame != Device.dwFrame)
        {
            torso_frame = Device.dwFrame;
            float head = 0.f;
            if (vr::HeadHeading(direction.x, direction.y, direction.z, up.x, up.z, head))
            {
                torso_yaw = torso_valid ? vr::TorsoYaw(torso_yaw, head, deg2rad(vr_torso_deadband)) : head;
                // Looking down more than 35 degrees: at the belt.
                torso_yaw = vr::TorsoSettle(torso_yaw, head, Device.fTimeDelta, direction.y < -0.57f);
                torso_valid = true;
            }
            // Meta body tracking: the torso faces as the real one does,
            // square to the shoulders. The headset's estimate carries on
            // from there when it is lost.
            float chest = 0.f;
            const float weight = BodyTrackingWeight();
            if (weight > 0.f && TrackedChestYaw(chest))
            {
                torso_yaw = torso_valid ? vr::BlendAngle(torso_yaw, chest, weight) : chest;
                torso_valid = true;
            }
        }
        if (!torso_valid) return false;
        const Fmatrix& body = system.BodyWorld();
        forward.set(0,0,0).mad(body.i, _sin(torso_yaw)).mad(body.k, _cos(torso_yaw));
        forward.y = 0;
        if (forward.square_magnitude() < EPS_S) return false;
        forward.normalize();
        return true;
    }
    void UpdateGameplayBody()
    {
        // Simulation sees this frame's body: hands, held items and the fire
        // point are placed with the body the eyes will be drawn with.
        if (CVrSystem::Get().IsRendering() && g_pGameLevel) PlaceBody();
    }
    void ShiftTrackingOrigin(const Fvector& world_delta)
    {
        if (GameplayActive()) CVrSystem::Get().ShiftTrackingOrigin(world_delta);
    }
    bool Roomscale() { return vr_roomscale != 0; }
    bool HeadPosition(Fvector& position)
    {
        if (!GameplayActive()) return false;
        Fvector direction, up;
        if (!CVrSystem::Get().HeadPose(position,direction,up)) return false;
        position=CVrSystem::Get().WorldHeadPosition();
        return true;
    }
    bool HeadHeightDelta(float& metres)
    {
        metres = 0.f;
        if (!GameplayActive()) return false;
        Fvector position, direction, up;
        if (!CVrSystem::Get().HeadPose(position,direction,up)) return false;
        metres = position.y/_max(vr_world_scale, 0.01f);
        return true;
    }
    bool GameplayActive()
    {
        return CVrSystem::Get().GameplayReady() && VrInput::Get().HasRecentState() &&
            !Device.Paused() && !Console->bVisible && g_pGameLevel &&
            !MainMenuActive();
    }
    unsigned GameplayInputEpoch() { return gameplay_input_epoch; }
    unsigned BodyEpoch() { return body_epoch; }
    bool MovementInput(float dt, float& side, float& forward, float& turn)
    {
        side = forward = turn = 0;
        if (!vr_controller_movement || !GameplayActive()) { turn_state.Reset(); comfort_motion = 0.f; return false; }
        auto& input = VrInput::Get();
        if (input.HandActive(MoveStick())) input.Thumbstick(MoveStick(), side, forward);
        FilterStick(side, forward, vr_move_deadzone);
        float x = 0, y = 0;
        if (input.HandActive(TurnStick())) input.Thumbstick(TurnStick(), x, y);
        else turn_state.Reset();
        turn = turn_state.Step(x, dt, vr_snap_turn != 0, vr_smooth_turn != 0,
            deg2rad(vr_snap_turn_angle), deg2rad(vr_turn_speed));
        // Stick motion drives the comfort vignette; a snap turn flashes it.
        float motion = _sqrt(side*side + forward*forward);
        if (turn != 0 && vr_smooth_turn)
            motion = _max(motion, clampr(_abs(turn)/(deg2rad(vr_turn_speed)*_max(dt, 0.001f)), 0.f, 1.f));
        else if (turn != 0) { comfort_snapped = true; comfort_snap_time = Device.dwTimeGlobal; }
        comfort_motion = _min(motion, 1.f);
        comfort_motion_time = Device.dwTimeGlobal;
        return true;
    }
    void SuspendGameplayInput() { turn_state.Reset(); stance_armed=false; comfort_motion=0.f; }
    u32 stance_centred_since=0;
    int StanceStep()
    {
        auto& input=VrInput::Get();
        // The stick's up/down belongs to an open window (the PDA map zoom,
        // scrolling) while the pointer is on it.
        if (!GameplayActive() || ui_pointer_owns_buttons || !input.HandActive(TurnStick()))
        { stance_armed=false; stance_centred_since=0; return 0; }
        float x=0, y=0;
        input.Thumbstick(TurnStick(),x,y);
        // One step per push; the stick must rest at the centre for 150 ms
        // first, so its spring back past the centre after a push down is not
        // taken for a push up. A mostly sideways push is a turn, not a
        // stance change.
        if (_abs(y)<0.35f)
        {
            if (!stance_centred_since) stance_centred_since=Device.dwTimeGlobal|1;
            if (Device.dwTimeGlobal-stance_centred_since>=150) stance_armed=true;
            return 0;
        }
        stance_centred_since=0;
        if (!stance_armed || _abs(y)<0.75f || _abs(y)<1.5f*_abs(x)) return 0;
        stance_armed=false;
        return y>0 ? 1 : -1;
    }
    // Each hand's grip and aim poses smoothed in tracking space (so walking
    // and turning do not lag), once a frame: vr_controller_smoothing is the
    // time constant in hundredths of a second.
    struct SmoothedPose { u32 frame=u32(-1); bool valid=false; XrPosef pose{}; XrVector3f last_raw{}; };
    SmoothedPose smoothed_hands[2][2];
    // tau: the time constant (s); 0 or less takes the pose as it is.
    XrPosef SmoothHandPose(SmoothedPose& state, const XrPosef& raw, float tau)
    {
        if (state.frame == Device.dwFrame) return state.pose;
        // A gap of more than a frame (a menu, the console, lost tracking)
        // starts afresh rather than sliding over from where it was.
        const bool resumed = state.frame == u32(-1) || Device.dwFrame - state.frame > 1;
        state.frame = Device.dwFrame;
        // A jump of the real controller (tracking regained), not how far the
        // smoothed pose trails a fast hand: that trail is the smoothing.
        const XrVector3f& p = state.last_raw;
        const float jump = (raw.position.x-p.x)*(raw.position.x-p.x) + (raw.position.y-p.y)*(raw.position.y-p.y) +
            (raw.position.z-p.z)*(raw.position.z-p.z);
        state.last_raw = raw.position;
        // Fresh, or a jump: taken as it is.
        if (!state.valid || resumed || tau <= 0.f || jump > 0.25f)
        {
            state.valid = true;
            state.pose = raw;
            return raw;
        }
        const float k = 1.f - expf(-clampr(Device.fTimeDelta, 0.f, 0.1f) / tau);
        XrPosef& s = state.pose;
        s.position.x += (raw.position.x - s.position.x) * k;
        s.position.y += (raw.position.y - s.position.y) * k;
        s.position.z += (raw.position.z - s.position.z) * k;
        // Rotation: the shorter way round, then normalised.
        XrQuaternionf q = raw.orientation;
        const float dot = s.orientation.x*q.x + s.orientation.y*q.y + s.orientation.z*q.z + s.orientation.w*q.w;
        if (dot < 0.f) { q.x = -q.x; q.y = -q.y; q.z = -q.z; q.w = -q.w; }
        s.orientation.x += (q.x - s.orientation.x) * k;
        s.orientation.y += (q.y - s.orientation.y) * k;
        s.orientation.z += (q.z - s.orientation.z) * k;
        s.orientation.w += (q.w - s.orientation.w) * k;
        const float length = _sqrt(s.orientation.x*s.orientation.x + s.orientation.y*s.orientation.y +
            s.orientation.z*s.orientation.z + s.orientation.w*s.orientation.w);
        if (length > 1e-6f)
        {
            s.orientation.x /= length; s.orientation.y /= length;
            s.orientation.z /= length; s.orientation.w /= length;
        }
        else s.orientation = raw.orientation;
        return s;
    }
    // Seated reach (vr_seated_reach). The game's floor under the actor's
    // eyes, as the game last placed the body (SetGameFloor), and what is
    // mapped between, in metres in the body's frame (the calibrated eye at
    // 0): the real floor, the game's floor and where the mapping starts.
    float game_floor_world = 0.f;
    u32 game_floor_frame = u32(-1);
    struct SeatedReachState
    {
        u32 frame = u32(-1);
        float amount = 0.f; // eased 0..1
        float real_floor = 0.f, game_floor = 0.f, start = 0.f;
    } seated;
    void SetGameFloor(float world_y) { game_floor_world = world_y; game_floor_frame = Device.dwFrame; }
    void UpdateSeatedReach()
    {
        if (seated.frame == Device.dwFrame) return;
        seated.frame = Device.dwFrame;
        auto& system = CVrSystem::Get();
        const float scale = _max(vr_world_scale, 0.01f);
        float real_floor = 0.f;
        Fvector head, direction, up;
        bool applies = false;
        if (vr_seated_reach && GameplayActive() && Device.dwFrame - game_floor_frame <= 1 &&
            system.RealFloorInBody(real_floor) && system.HeadPose(head, direction, up))
        {
            const float real_m = real_floor / scale;
            const float game_m = (game_floor_world - system.BodyPlacement().c.y) / scale;
            applies = vr::SeatedReachApplies(vr_seated_reach, real_m, game_m);
            if (applies)
            {
                seated.real_floor = real_m;
                seated.game_floor = game_m;
                seated.start = vr::SeatedReachStart(real_m, head.y / scale, vr_hip_height);
            }
        }
        // Eased over half a second: the eye height taken again (standing
        // up, sitting down) never snaps the hands.
        const float step = clampr(Device.fTimeDelta, 0.f, 0.1f) / 0.5f;
        seated.amount = clampr(seated.amount + (applies ? step : -step), 0.f, 1.f);
    }
    // What seated reach adds to a tracked point's height, in tracking
    // metres (0 or less): a hand going down toward the real floor reaches
    // the game's floor.
    float SeatedReachOffset(const XrPosef& pose)
    {
        UpdateSeatedReach();
        if (seated.amount <= 0.f) return 0.f;
        const float y = CVrSystem::Get().TrackingToBody(pose).c.y / _max(vr_world_scale, 0.01f);
        return seated.amount * vr::SeatedReachDrop(y, seated.real_floor, seated.game_floor, seated.start);
    }
    // The hand's tracking pose this frame, smoothed (vr_controller_smoothing),
    // and taken down toward the game's floor when reaching down seated.
    bool HandPose(int hand, bool aim, XrPosef& pose)
    {
        if (!GameplayActive()) return false;
        if (!(aim ? VrInput::Get().AimPose(hand, pose) : VrInput::Get().GripPose(hand, pose)))
        {
            if (hand >= 0 && hand < 2) smoothed_hands[hand][aim ? 1 : 0].valid = false;
            return false;
        }
        if (hand >= 0 && hand < 2 && vr_controller_smoothing > 0.f)
            pose = SmoothHandPose(smoothed_hands[hand][aim ? 1 : 0], pose, vr_controller_smoothing * 0.01f);
        pose.position.y += SeatedReachOffset(pose);
        return true;
    }
    bool HandTransform(int hand, bool aim, Fmatrix& world)
    {
        XrPosef pose;
        if (!HandPose(hand, aim, pose)) return false;
        world = CVrSystem::Get().TrackingToWorld(pose);
        return true;
    }
    bool HandInBody(int hand, bool aim, Fmatrix& local)
    {
        XrPosef pose;
        if (!HandPose(hand, aim, pose)) return false;
        local = CVrSystem::Get().TrackingToBody(pose);
        return true;
    }
    // Meta body tracking: the chest, then each side's shoulder joint, elbow
    // and wrist (left, right), as XrBodyJointFB.
    const int body_joint_ids[7] = {XR_BODY_JOINT_CHEST_FB,
        XR_BODY_JOINT_LEFT_ARM_UPPER_FB, XR_BODY_JOINT_RIGHT_ARM_UPPER_FB,
        XR_BODY_JOINT_LEFT_ARM_LOWER_FB, XR_BODY_JOINT_RIGHT_ARM_LOWER_FB,
        XR_BODY_JOINT_LEFT_HAND_WRIST_FB, XR_BODY_JOINT_RIGHT_HAND_WRIST_FB};
    struct BodyTrackingState
    {
        u32 frame = u32(-1);
        float weight = 0.f;
        // Body tracking is in use (weight above 0): both shoulders were
        // tracked, now or until the weight eased out.
        bool have = false;
        // Each joint: located this frame, and a pose kept from when it was.
        bool valid[7]{}, seen[7]{};
        bool shoulders = false; // both shoulders located this frame
        SmoothedPose smoothed[7];
        XrPosef pose[7]{};
        vr::BodySizeSampler size;
        u32 debug_time = 0;
    } body_state;
    int BodyJointSlot(BodyJoint joint, int side)
    {
        if (joint == BodyJointChest) return 0;
        if (side < 0 || side > 1 || joint < BodyJointShoulder || joint > BodyJointWrist) return -1;
        return 1 + (int(joint) - int(BodyJointShoulder)) * 2 + side;
    }
    // vr_body_tracking_debug: once a second, whatever the state, so a body
    // that is not used says why.
    void LogBodyJoints()
    {
        if (!vr_body_tracking_debug || Device.dwTimeGlobal - body_state.debug_time < 1000) return;
        body_state.debug_time = Device.dwTimeGlobal;
        auto& system = CVrSystem::Get();
        const int fidelity = system.BodyTrackingFidelity();
        Msg("* [VR body] tracker: tracked=%d confidence=%.2f fidelity=%s | used: weight=%.2f | located: chest=%d "
            "L shoulder=%d elbow=%d wrist=%d, R shoulder=%d elbow=%d wrist=%d | size: width=%.2f arm=%.2f (%d samples) "
            "seated=%.2f",
            int(system.BodyTrackingTracked()), system.BodyTrackingConfidence(),
            fidelity == XR_BODY_TRACKING_FIDELITY_HIGH_META ? "high" : fidelity == XR_BODY_TRACKING_FIDELITY_LOW_META ?
            "low" : "unknown", body_state.have ? body_state.weight : 0.f, int(body_state.valid[0]),
            int(body_state.valid[1]), int(body_state.valid[3]), int(body_state.valid[5]), int(body_state.valid[2]),
            int(body_state.valid[4]), int(body_state.valid[6]), body_state.size.Width(), body_state.size.Arm(),
            body_state.size.Count(), seated.amount);
        Fvector head, direction, up;
        if (!body_state.have || !system.HeadPose(head, direction, up)) return;
        // Relative to the eyes, in the body's frame (m; x right, y up, z ahead).
        const float scale = _max(vr_world_scale, 0.01f);
        const auto at = [&](int slot)
        {
            Fvector p = system.TrackingToBody(body_state.pose[slot]).c;
            return p.sub(head).div(scale);
        };
        const Fvector ls = at(1), rs = at(2), le = at(3), re = at(4), lw = at(5), rw = at(6);
        Msg("* [VR body] L: shoulder=(%.2f %.2f %.2f) elbow=(%.2f %.2f %.2f) wrist=(%.2f %.2f %.2f) "
            "R: shoulder=(%.2f %.2f %.2f) elbow=(%.2f %.2f %.2f) wrist=(%.2f %.2f %.2f)",
            ls.x, ls.y, ls.z, le.x, le.y, le.z, lw.x, lw.y, lw.z, rs.x, rs.y, rs.z, re.x, re.y, re.z, rw.x, rw.y, rw.z);
    }
    // Once a frame: this frame's tracked joints (smoothed), how far they are
    // trusted, and the player's measured size. Each joint stands alone: the
    // body is used while both shoulders are tracked; an elbow or wrist the
    // runtime does not locate is only left out.
    void UpdateBodyTracking()
    {
        if (body_state.frame == Device.dwFrame) return;
        const float dt = body_state.frame == u32(-1) ? 0.f : clampr(Device.fTimeDelta, 0.f, 0.1f);
        body_state.frame = Device.dwFrame;
        auto& system = CVrSystem::Get();
        const bool tracked = vr_body_tracking && GameplayActive() && system.BodyTrackingTracked();
        // The headset's estimate of elbows and shoulders shakes more than
        // the controllers do: at least 50 ms of smoothing.
        const float tau = _max(vr_controller_smoothing * 0.01f, 0.05f);
        XrPosef raw[7];
        for (int i = 0; i < 7; ++i)
        {
            body_state.valid[i] = tracked && system.BodyJoint(body_joint_ids[i], raw[i]);
            if (!body_state.valid[i]) continue;
            body_state.pose[i] = SmoothHandPose(body_state.smoothed[i], raw[i], tau);
            body_state.seen[i] = true;
        }
        body_state.shoulders = body_state.valid[1] && body_state.valid[2];
        // The player's size, from the joints as tracked (m), when the arms are.
        bool arms = true;
        for (int i = 1; i < 7; ++i) arms = arms && body_state.valid[i];
        if (arms)
        {
            const auto at = [&](int i) { return vr::BodyVec(raw[i].position.x, raw[i].position.y, raw[i].position.z); };
            const float width = vr::BodyLength(at(1) - at(2));
            const float arm = 0.5f * (vr::BodyLength(at(1) - at(3)) + vr::BodyLength(at(3) - at(5)) +
                vr::BodyLength(at(2) - at(4)) + vr::BodyLength(at(4) - at(6)));
            const bool settled = body_state.size.Settled();
            body_state.size.Add(width, arm, dt);
            if (!settled && body_state.size.Settled())
                Msg("* [VR] body tracking: measured shoulder width %.2f m, arm length %.2f m",
                    body_state.size.Width(), body_state.size.Arm());
        }
        body_state.weight = vr::TrackingWeight(body_state.weight, body_state.shoulders, dt);
        body_state.have = body_state.weight > 0.f && body_state.seen[1] && body_state.seen[2];
        if (body_state.weight <= 0.f)
            for (bool& seen : body_state.seen) seen = false;
        LogBodyJoints();
    }
    // The tracked torso's yaw in the body's frame, square to its shoulders.
    bool TrackedChestYaw(float& yaw)
    {
        UpdateBodyTracking();
        if (!body_state.have) return false;
        auto& system = CVrSystem::Get();
        const Fvector left = system.TrackingToBody(body_state.pose[1]).c;
        const Fvector right = system.TrackingToBody(body_state.pose[2]).c;
        return vr::ChestYaw(vr::BodyVec(left.x, left.y, left.z), vr::BodyVec(right.x, right.y, right.z), yaw);
    }
    bool BodyJointPosition(BodyJoint joint, int side, Fvector& world)
    {
        if (!GameplayActive()) return false;
        UpdateBodyTracking();
        const int slot = BodyJointSlot(joint, side);
        // A joint located this frame; or, while the body eases out, as it
        // was last located. Never a stale one while the rest is tracked.
        if (!body_state.have || slot < 0 || !body_state.seen[slot] ||
            (!body_state.valid[slot] && body_state.shoulders)) return false;
        // Taken down with the hands when reaching down seated.
        XrPosef pose = body_state.pose[slot];
        pose.position.y += SeatedReachOffset(pose);
        world = CVrSystem::Get().TrackingToWorld(pose).c;
        return true;
    }
    float BodyTrackingWeight()
    {
        if (!GameplayActive()) return 0.f;
        UpdateBodyTracking();
        return body_state.have ? body_state.weight : 0.f;
    }
    float ShoulderWidthScale()
    {
        UpdateBodyTracking();
        const float width = vr_shoulder_width > 0.f ? vr_shoulder_width :
            body_state.size.Settled() ? body_state.size.Width() : 0.f;
        return vr::BodySizeFactor(width, vr::average_shoulder_width);
    }
    float ArmLength()
    {
        UpdateBodyTracking();
        const float arm = vr_arm_length > 0.f ? vr_arm_length :
            body_state.size.Settled() ? body_state.size.Arm() : 0.f;
        return arm * vr_world_scale;
    }
    bool BodyTransform(Fmatrix& world)
    {
        if (!GameplayActive()) return false;
        world = CVrSystem::Get().BodyPlacement();
        return true;
    }
    float HandSqueeze(int hand) { return GameplayActive() && !LoadingControlBlocked(hand,2u) ? VrInput::Get().Grip(hand) : 0.f; }
    float HandTrigger(int hand)
    {
        if (!GameplayActive() || ui_pointer_owns_buttons || LoadingControlBlocked(hand,1u)) return 0.f;
        auto& input=VrInput::Get();
        // The analog travel whenever it is bound: the Boolean select on the
        // same trigger turns true past the runtime's threshold, and read as
        // 1 it hid partial releases (1 > 0.75 > 1), so FireTrigger's reset
        // never saw a quick tap. The select only without an analog trigger.
        if (input.TriggerAnalog(hand)) return input.Trigger(hand);
        return input.Select(hand) ? 1.f : 0.f;
    }
    bool HandSecondaryButton(int hand)
    {
        return GameplayActive() && !ui_pointer_owns_buttons && VrInput::Get().SecondaryButton(hand);
    }
    bool HandButton(int hand)
    {
        return GameplayActive() && !ui_pointer_owns_buttons && !LoadingControlBlocked(hand,4u) &&
            VrInput::Get().Button(hand);
    }
    // Sprint is the movement stick's click, jump the turning stick's.
    bool SprintButton()
    {
        auto& input=VrInput::Get();
        return GameplayActive() && (vr_left_handed ? input.JumpButton() : input.SprintButton());
    }
    bool JumpButton()
    {
        auto& input=VrInput::Get();
        return GameplayActive() && !ui_pointer_owns_buttons &&
            (vr_left_handed ? input.SprintButton() : input.JumpButton());
    }
    void HandHaptic(int hand, float amplitude) { if (GameplayActive()) VrInput::Get().ApplyHaptic(hand, amplitude, 25000000); }
    bool HandsVisible() { return vr_hands != 0; }
    bool AnimatedGrip() { return vr_animated_grip != 0; }
    bool MechanicalFireRate() { return vr_mechanical_fire_rate != 0; }
    int PhysicalHands() { return vr_physical_hands; }
    int ContactDebug() { return vr_contact_debug; }
    bool BodyVisible() { return vr_body != 0; }
    bool PhysicalWeapon() { return vr_physical_weapon != 0; }
    void SetScopeView(const Fmatrix& camera, float fov, float lens, LPCSTR reticle, const float* reticle_rect)
    {
        scope_camera = camera; scope_fov = fov; scope_lens = lens; scope_request_frame = Device.dwFrame;
        // The lens shows the plain duplex crosshair, as at HEAD (28/10), when
        // scopes last worked well: the scope's own reticle overlay, added
        // after, broke them and is set aside for now.
        (void)reticle; (void)reticle_rect;
        scope_reticle = "";
        scope_reticle_part = false;
        for (float& value : scope_reticle_rect) value = 0.f;
    }
    float ScopeZoomScale() { return vr_scope_zoom; }
    float ScopeLensScale() { return vr_scope_size; }
    static void DrawLensQuad(const Fvector& centre, const Fvector& right, const Fvector& up, const float* params)
    {
        const Fvector corners[4] = {
            Fvector().add(centre, Fvector().sub(up, right)),
            Fvector().add(centre, Fvector().add(up, right)),
            Fvector().sub(centre, Fvector().add(up, right)),
            Fvector().add(centre, Fvector().sub(right, up))};
        float clip[16];
        for (int i = 0; i < 4; ++i) ClipPosition(Device.mFullTransform, corners[i], clip + i * 4);
        vr_scope_lens(clip, params);
        vr_world_overlay();
    }
    bool DrawScopeLens(const Fvector& centre, const Fvector& right, const Fvector& up,
        float brightness, float offset_x, float offset_y)
    {
        if (Device.dwFrame - scope_ready_frame > 4 || Device.dwFrame - scope_request_frame > 1) return false;
        const float params[5] = {brightness, offset_x, offset_y, 0.f, 0.f};
        vr_scope_reticle(scope_reticle.size() ? scope_reticle.c_str() : nullptr,
            scope_reticle_part ? scope_reticle_rect : nullptr);
        DrawLensQuad(centre, right, up, params);
        return true;
    }
    void DrawReflexDot(const Fvector& centre, const Fvector& right, const Fvector& up, float alpha,
        bool depth)
    {
        const float params[5] = {alpha, 0.f, 0.f, depth ? 1.f : 0.f, 1.f};
        DrawLensQuad(centre, right, up, params);
    }
    float UiScrollAxis(bool move_stick_too)
    {
        auto& input=VrInput::Get();
        if (!CVrSystem::Get().HasInputFocus() || !input.HasRecentState()) return 0.f;
        float best=0.f;
        for (const int stick:{TurnStick(),MoveStick()})
        {
            if (stick==MoveStick() && !move_stick_too) continue;
            if (!input.HandActive(stick)) continue;
            float x=0, y=0;
            input.Thumbstick(stick,x,y);
            if (_abs(y)>1.5f*_abs(x) && _abs(y)>_abs(best)) best=y;
        }
        return best;
    }
    bool HandRaysVisible() { return vr_hand_rays != 0; }
    float MaxHeadOffset() { return vr_max_head_offset; }
    bool PhysicalCrouch() { return vr_physical_crouch != 0; }
    float CrouchDepth() { return vr_crouch_depth; }
    float LowCrouchDepth() { return vr_low_crouch_depth; }
    bool BodySlots() { return vr_body_slots != 0; }
    bool BodySlotRings() { return vr_slot_rings != 0; }
    float BodySlotRadius() { return vr_slot_radius; }
    float HipHeight() { return vr_hip_height; }
    float HipWidth() { return vr_hip_width; }
    bool LeftHanded() { return vr_left_handed != 0; }
    int DominantHand() { return vr_left_handed ? 0 : 1; }
    bool WristDisplay() { return vr_wrist_display != 0; }
    float MessageTextSize() { return vr_message_text_size; }
    bool HideHud() { return vr_hide_hud != 0; }
    float HudMagnification() { return vr_hud_distance > 0.f ? vr_hud_scale/vr_hud_distance : 1.f; }
    bool WorldToHud(const Fvector& world, float& u, float& v, float& depth, bool& inside)
    {
        u=v=-1.f; depth=0.f; inside=false;
        auto& system=CVrSystem::Get();
        Fmatrix head;
        // The menu's panel stays where it was opened in the room: not the HUD.
        if (!GameplayActive() || system.UiPanelAnchored() || !system.HeadWorld(head)) return false;
        Fmatrix to_head; to_head.invert(head);
        Fvector local; to_head.transform_tiny(local,world);
        depth=local.z;
        float width=0.f, distance=0.f;
        system.UiPanel(width,distance);
        return vr::ProjectOntoQuad(local,distance,width,width*vr::kUiPanelAspect,u,v,inside);
    }
    float ThrowScale() { return vr_throw_scale; }
    float Recoil() { return vr_recoil; }
    float LabelRange() { return vr_label_range; }
    float NpcLabelRange() { return vr_npc_label_range; }
    void Recenter() { ++gameplay_input_epoch; torso_valid = false; CVrSystem::Get().Recenter(); }
    void CalibrateHeight() { CVrSystem::Get().RecalibrateHeight(); Msg("* [VR] eye height taken from the head now"); }
    void RequestQuickSave() { if (GameplayActive()) quick_save_pending = true; }
    void ForgetGameplayBody(const void* owner) { CVrSystem::Get().ForgetBody(owner); torso_valid = false; }

}
#else
namespace vr
{
    void HideHandForFrame(int) {}
    bool HandHiddenForFrame(int) { return false; }
    bool HeadDirectionInBody(Fvector&) { return false; }
    bool HeadTransform(Fmatrix&) { return false; }
    bool TorsoForward(Fvector&) { return false; }
    bool BodyJointPosition(BodyJoint, int, Fvector&) { return false; }
    float BodyTrackingWeight() { return 0.f; }
    void UpdateBodyTracking() {}
    float ShoulderWidthScale() { return 1.f; }
    float ArmLength() { return 0.f; }
    void SetGameFloor(float) {}
    void UpdateGameplayBody() {}
    void ShiftTrackingOrigin(const Fvector&) {}
    bool Roomscale() { return false; }
    bool HeadPosition(Fvector&) { return false; }
    bool GameplayActive() { return false; }
    unsigned GameplayInputEpoch() { return 0; }
    unsigned BodyEpoch() { return 0; }
    bool MovementInput(float, float& side, float& forward, float& turn) { side = forward = turn = 0; return false; }
    void SuspendGameplayInput() {}
    int StanceStep() { return 0; }
    bool HandTransform(int, bool, Fmatrix&) { return false; }
    bool HandInBody(int, bool, Fmatrix&) { return false; }
    void ForgetGameplayBody(const void*) {}
    bool BodyTransform(Fmatrix&) { return false; }
    float HandSqueeze(int) { return 0; }
    float HandTrigger(int) { return 0; }
    bool HandSecondaryButton(int) { return false; }
    bool HandButton(int) { return false; }
    bool SprintButton() { return false; }
    bool JumpButton() { return false; }
    void HandHaptic(int, float) {}
    bool HandsVisible() { return false; }
    bool AnimatedGrip() { return false; }
    bool MechanicalFireRate() { return false; }
    int PhysicalHands() { return 0; }
    int ContactDebug() { return 0; }
    bool BodyVisible() { return false; }
    bool PhysicalWeapon() { return false; }
    void SetScopeView(const Fmatrix&, float, float, LPCSTR, const float*) {}
    bool DrawScopeLens(const Fvector&, const Fvector&, const Fvector&, float, float, float) { return false; }
    void DrawReflexDot(const Fvector&, const Fvector&, const Fvector&, float, bool) {}
    float ScopeZoomScale() { return 1.f; }
    float ScopeLensScale() { return 1.f; }
    float UiScrollAxis(bool) { return 0.f; }
    bool HandRaysVisible() { return false; }
    float MaxHeadOffset() { return 0; }
    bool HeadHeightDelta(float& metres) { metres = 0.f; return false; }
    bool PhysicalCrouch() { return false; }
    float CrouchDepth() { return 0.30f; }
    float LowCrouchDepth() { return 0.60f; }
    bool BodySlots() { return false; }
    bool BodySlotRings() { return false; }
    float BodySlotRadius() { return 0.10f; }
    float HipHeight() { return default_hip_height; }
    float HipWidth() { return 0.22f; }
    bool LeftHanded() { return false; }
    int DominantHand() { return 1; }
    bool WristDisplay() { return false; }
    float MessageTextSize() { return 1.f; }
    bool HideHud() { return false; }
    float HudMagnification() { return 1.f; }
    bool WorldToHud(const Fvector&, float& u, float& v, float& depth, bool& inside)
    { u=v=-1.f; depth=0.f; inside=false; return false; }
    float ThrowScale() { return 1.75f; }
    float Recoil() { return 1.f; }
    float LabelRange() { return 0.f; }
    float NpcLabelRange() { return 0.f; }
    void RequestQuickSave() {}
}
#endif
