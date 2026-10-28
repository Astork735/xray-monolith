#pragma once

class CActor;
class IKinematicsAnimated;
class IKinematics;
struct IDSGraphManager;

class player_legs_controller
{
public:
    void    update(CActor* actor, bool isShadowPass = false);
    void    render(bool isShadowPass = false);
    void    destroy();

    bool    is_active() const { return m_model != nullptr; }
    bool    vr_outfit_arms() const { return m_vr_outfit_arms && m_model != nullptr; }
    IKinematics* model() const { return m_model; }
    void    collapse_vr_body();
    // World pose of the tracked right hand and finger2 bones from this or the
    // previous frame, for attaching an unprofiled weapon as NPCs hold it.
    bool    vr_weapon_hand(int side, Fmatrix& hand, Fmatrix& finger) const;

private:
    IKinematics* m_model = nullptr;
    IKinematics* m_shadow_model = nullptr;
    shared_str              m_visual_name, m_shadow_name, m_vr_body_visual;
    shared_str              m_last_outfit_sect;
    shared_str              m_last_model;
    std::optional<float> m_fwd_offset = std::nullopt;
    float                   m_y_offset = 0.f;
    Fmatrix                 m_legs_transform;
    bool                    m_first_frame = true;
    xr_vector<Fmatrix>      m_prev_bones;
    bool                    m_has_prev = false;
    float                   m_blend_speed = 8.f;
    bool                    m_logged = false;
    bool                    m_vr_outfit_arms = false;
    bool                    m_last_vr_mode = false;
    bool                    m_vr_model_mode = false;
    u16                     m_vr_arm_bones[2][3]{};
    // Per bone: 0 body (skinning collapsed in VR), 1 left arm, 2 right arm.
    xr_vector<u8>           m_vr_bone_group;
    xr_vector<u8>           m_vr_collapse_side;
    xr_vector<u8>           m_vr_finger;        // 0 thumb, 1 index, 2 others, 255 none
    xr_vector<u8>           m_vr_finger_joint;
    xr_vector<u8>           m_vr_digit;         // 0 thumb .. 4 little finger
    xr_vector<Fmatrix>      m_vr_bind_local;
    xr_vector<Fvector>      m_vr_segment_local;
    xr_vector<Fmatrix>      m_vr_bone_poses;
    xr_vector<u16>          m_vr_arm_order[2];  // below the wrist, parents first
    Fvector                 m_vr_hand_basis[2][3];
    // Per hand and finger (thumb, index, others): the hinge axis in hand
    // space, fixed so a deeply curled joint cannot flip its bend direction.
    Fvector                 m_vr_curl_axis[2][3]{};
    u16                     m_vr_finger2[2]{u16(-1),u16(-1)};
    u16                     m_vr_index[2]{u16(-1),u16(-1)};
    // Arms are solved once per tracked body placement (vr::BodyEpoch).
    unsigned                m_vr_solved_epoch=0;
    bool                    m_vr_solved=false, m_vr_solving=false;
    // Midpoint of the index and middle knuckles in hand-bone space.
    Fvector                 m_vr_knuckle_local[2]{};
    xr_vector<u8>           m_vr_upper_arm;     // 1 left, 2 right upper arm
    xr_vector<Fvector>      m_vr_contact;   // weapon mesh points near a hand
    Fmatrix                 m_vr_weapon_hand[2], m_vr_weapon_finger[2];
    u32                     m_vr_weapon_hand_frame[2]{};
    bool                    m_vr_hand_basis_valid[2]{};
    Fmatrix                 m_vr_arms_transform=Fidentity;
    // Bind-pose arm chain in an upright, forward-facing frame centred on the
    // shoulders, so running or crouch animations never move the arms.
    Fmatrix                 m_vr_rest[2][3];
    void                    prepare_vr_arms();
    void                    anchor_vr_arms();
    void                    pose_vr_arms(CActor* actor);
    bool                    refresh_vr_arms(CActor* actor);
    void                    apply_vr_arm_bones();
    bool is_costume_affected_bone(LPCSTR bone_name) const;
    bool should_hide_bone(LPCSTR bone_name) const;
    bool is_spine_bone(LPCSTR bone_name) const;
    void remove_camera_pitch(Fmatrix& bone_transform);

    bool    resolve_config(CActor* actor, shared_str& sect, shared_str& model);
    bool    ensure_model(const shared_str& sect, const shared_str& model);
    void    copy_bones_from_actor(CActor* actor);
    void    warn_once(const char* fmt, ...);
    bool is_keep_bind_bone(LPCSTR bone_name) const;
};
