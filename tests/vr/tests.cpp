#include "support.h"
#include "../../src/xrEngine/Vr/vr_system.h"
#include "../../src/xrEngine/Vr/vr_input.h"
#include "../../src/xrEngine/Vr/vr_tracking.h"
#include "../../src/xrEngine/Vr/vr_config.h"
#include "../../src/xrEngine/Vr/vr_policy.h"
#include "../../src/xrEngine/Vr/vr_gameplay_policy.h"
#include "../../src/xrEngine/Vr/vr_hand_render.h"
#include "../../src/xrEngine/Vr/vr_frame_work.h"
#include "../../src/xrEngine/Vr/vr_ui_policy.h"
#include "../../src/xrPhysics/CameraProbeState.h"
#include "../../src/xrGame/VrPistolPose.h"
#include "../../src/xrGame/VrWeaponPose.h"
#include "ak12_fixture.h"
#include <array>
#include <functional>
#include <cstdlib>
#include <string>

PFN_xrGetInstanceProcAddr xrGetInstanceProcAddr_p = nullptr;
PFN_xrCreateInstance xrCreateInstance_p = nullptr;
PFN_xrEnumerateInstanceExtensionProperties xrEnumerateInstanceExtensionProperties_p = nullptr;
PFN_xrGetInstanceProperties xrGetInstanceProperties_p = nullptr;
PFN_xrGetSystem xrGetSystem_p = nullptr;
PFN_xrCreateSession xrCreateSession_p = nullptr;
PFN_xrDestroySession xrDestroySession_p = nullptr;
PFN_xrDestroyInstance xrDestroyInstance_p = nullptr;
PFN_xrCreateReferenceSpace xrCreateReferenceSpace_p = nullptr;
PFN_xrDestroySpace xrDestroySpace_p = nullptr;
PFN_xrBeginSession xrBeginSession_p = nullptr;
PFN_xrEndSession xrEndSession_p = nullptr;
PFN_xrPollEvent xrPollEvent_p = nullptr;
PFN_xrWaitFrame xrWaitFrame_p = nullptr;
PFN_xrBeginFrame xrBeginFrame_p = nullptr;
PFN_xrLocateViews xrLocateViews_p = nullptr;
PFN_xrEndFrame xrEndFrame_p = nullptr;
PFN_xrEnumerateSwapchainFormats xrEnumerateSwapchainFormats_p = nullptr;
PFN_xrEnumerateViewConfigurationViews xrEnumerateViewConfigurationViews_p = nullptr;
PFN_xrCreateSwapchain xrCreateSwapchain_p = nullptr;
PFN_xrEnumerateSwapchainImages xrEnumerateSwapchainImages_p = nullptr;
PFN_xrDestroySwapchain xrDestroySwapchain_p = nullptr;
PFN_xrAcquireSwapchainImage xrAcquireSwapchainImage_p = nullptr;
PFN_xrWaitSwapchainImage xrWaitSwapchainImage_p = nullptr;
PFN_xrReleaseSwapchainImage xrReleaseSwapchainImage_p = nullptr;
PFN_xrCreateActionSet xrCreateActionSet_p = nullptr;
PFN_xrDestroyActionSet xrDestroyActionSet_p = nullptr;
PFN_xrStringToPath xrStringToPath_p = nullptr;
PFN_xrCreateAction xrCreateAction_p = nullptr;
PFN_xrCreateActionSpace xrCreateActionSpace_p = nullptr;
PFN_xrSuggestInteractionProfileBindings xrSuggestInteractionProfileBindings_p = nullptr;
PFN_xrSyncActions xrSyncActions_p = nullptr;
PFN_xrAttachSessionActionSets xrAttachSessionActionSets_p = nullptr;
PFN_xrGetActionStateBoolean xrGetActionStateBoolean_p = nullptr;
PFN_xrGetActionStateFloat xrGetActionStateFloat_p = nullptr;
PFN_xrGetActionStateVector2f xrGetActionStateVector2f_p = nullptr;
PFN_xrGetActionStatePose xrGetActionStatePose_p = nullptr;
PFN_xrLocateSpace xrLocateSpace_p = nullptr;
PFN_xrApplyHapticFeedback xrApplyHapticFeedback_p = nullptr;
TestDevice Device;
bool g_vr_owns_present = false;
TestPersistent* g_pGamePersistent = nullptr;
int vr_enabled=1,vr_tracking_origin=0,vr_debug=0,vr_snap_turn=1,vr_smooth_turn=0;
float vr_world_scale=1,vr_render_scale=1,vr_snap_turn_angle=30,vr_near_plane=0.03f;
int vr_depth_layer=1;
float vr_ui_distance=1.5f;
float vr_hud_distance=1.f, vr_hud_scale=1.f;
int vr_shared_shadows=1;
float vr_scope_quality=1.f;
EVrLaunchMode g_vr_launch_mode=VR_Launch_Auto;
#define REQUIRE(x) do{if(!(x)){std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
struct Runtime
{
    ID3D11Device* device=nullptr;
    LUID luid{};
    int instances=0,sessions=0,spaces=0,sets=0,actions=0,chains=0;
    int waits=0,begins=0,ends=0,syncs=0,acquires=0,releases=0,layers=0,projection_layers=0;
    bool attached=false,running=false,begun=false,active=true,render=true,tracking=true;
    int event=0;
    XrSessionState queued_state=XR_SESSION_STATE_UNKNOWN;
    float head_x=0,head_y=1.6f,head_yaw=0; bool space_change=false;
    XrSpace quad_space=XR_NULL_HANDLE; XrPosef quad_pose{}; float quad_width=0;
    bool loader=true,headset=true,no_ready=false,stopping=false;
    // The runtime offers XR_KHR_composition_layer_depth and D24S8 swapchains.
    bool depth=false; int depth_views=0;
    int binding_profiles=0;
    int fail_action=0,fail_chain=0;
    DXGI_FORMAT color_format=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    XrResult wait_image=XR_SUCCESS,wait_frame=XR_SUCCESS,begin_frame=XR_SUCCESS;
} runtime;
struct Chain {ID3D11Texture2D*texture=nullptr;bool acquired=false,waited=false;};
static std::vector<Chain*> chains;
static XrInstance Instance(){return reinterpret_cast<XrInstance>(1);}
static XrSession Session(){return reinterpret_cast<XrSession>(2);}
static XrResult XRAPI_PTR EnumerateExtensions(const char*,uint32_t capacity,uint32_t*count,XrExtensionProperties*out)
{
    *count=runtime.depth?2:1;
    if(capacity)strcpy_s(out[0].extensionName,XR_KHR_D3D11_ENABLE_EXTENSION_NAME);
    if(capacity>1&&runtime.depth)strcpy_s(out[1].extensionName,XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME);
    return XR_SUCCESS;
}
static XrResult XRAPI_PTR CreateInstance(const XrInstanceCreateInfo*,XrInstance*out)
{++runtime.instances;*out=Instance();return XR_SUCCESS;}
static XrResult XRAPI_PTR DestroyInstance(XrInstance){--runtime.instances;return XR_SUCCESS;}
static XrResult XRAPI_PTR Properties(XrInstance,XrInstanceProperties*out){strcpy_s(out->runtimeName,"Mock OpenXR");return XR_SUCCESS;}
static XrResult XRAPI_PTR GetSystem(XrInstance,const XrSystemGetInfo*,XrSystemId*out){if(!runtime.headset)return XR_ERROR_FORM_FACTOR_UNAVAILABLE;*out=1;return XR_SUCCESS;}
static XrResult XRAPI_PTR Graphics(XrInstance,XrSystemId,XrGraphicsRequirementsD3D11KHR*out)
{out->adapterLuid=runtime.luid;out->minFeatureLevel=D3D_FEATURE_LEVEL_11_0;return XR_SUCCESS;}
static XrResult XRAPI_PTR Proc(XrInstance,const char*,PFN_xrVoidFunction*out)
{*out=reinterpret_cast<PFN_xrVoidFunction>(Graphics);return XR_SUCCESS;}
static XrResult XRAPI_PTR CreateSession(XrInstance,const XrSessionCreateInfo*,XrSession*out)
{++runtime.sessions;runtime.event=0;*out=Session();return XR_SUCCESS;}
static XrResult XRAPI_PTR DestroySession(XrSession){--runtime.sessions;runtime.running=false;runtime.attached=false;return XR_SUCCESS;}
static XrResult XRAPI_PTR CreateSpace(XrSession,const XrReferenceSpaceCreateInfo*,XrSpace*out)
{*out=reinterpret_cast<XrSpace>(uintptr_t(10+ ++runtime.spaces));return XR_SUCCESS;}
static XrResult XRAPI_PTR DestroySpace(XrSpace){--runtime.spaces;return XR_SUCCESS;}
static XrResult XRAPI_PTR BeginSession(XrSession,const XrSessionBeginInfo*){runtime.running=true;return XR_SUCCESS;}
static XrResult XRAPI_PTR EndSession(XrSession){runtime.running=false;return XR_SUCCESS;}
static XrResult XRAPI_PTR Poll(XrInstance,XrEventDataBuffer*buffer)
{
    if(runtime.no_ready)return XR_EVENT_UNAVAILABLE;
    if(runtime.stopping)
    {
        runtime.stopping=false;
        auto*out=reinterpret_cast<XrEventDataSessionStateChanged*>(buffer);
        out->type=XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED;out->session=Session();out->state=XR_SESSION_STATE_STOPPING;return XR_SUCCESS;
    }
    if(runtime.space_change)
    {
        runtime.space_change=false;
        auto*out=reinterpret_cast<XrEventDataReferenceSpaceChangePending*>(buffer);
        out->type=XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING;out->session=Session();
        out->referenceSpaceType=XR_REFERENCE_SPACE_TYPE_LOCAL;out->changeTime=runtime.waits*10000000;return XR_SUCCESS;
    }
    if(runtime.queued_state!=XR_SESSION_STATE_UNKNOWN)
    {
        auto*out=reinterpret_cast<XrEventDataSessionStateChanged*>(buffer);
        out->type=XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED;out->session=Session();
        out->state=runtime.queued_state;runtime.queued_state=XR_SESSION_STATE_UNKNOWN;return XR_SUCCESS;
    }
    if(runtime.event>=2)return XR_EVENT_UNAVAILABLE;
    auto*out=reinterpret_cast<XrEventDataSessionStateChanged*>(buffer);
    out->type=XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED;out->session=Session();
    out->state=runtime.event++==0?XR_SESSION_STATE_READY:XR_SESSION_STATE_FOCUSED;return XR_SUCCESS;
}
static XrResult XRAPI_PTR WaitFrame(XrSession,const XrFrameWaitInfo*,XrFrameState*out)
{REQUIRE(runtime.running&&!runtime.begun);++runtime.waits;out->shouldRender=runtime.render;out->predictedDisplayTime=runtime.waits*10000000;return runtime.wait_frame;}
static XrResult XRAPI_PTR BeginFrame(XrSession,const XrFrameBeginInfo*)
{REQUIRE(!runtime.begun);++runtime.begins;runtime.begun=true;return runtime.begin_frame;}
static XrResult XRAPI_PTR EndFrame(XrSession,const XrFrameEndInfo*info)
{
    REQUIRE(runtime.begun);runtime.begun=false;++runtime.ends;runtime.layers=info->layerCount;runtime.projection_layers=0;
    runtime.depth_views=0;
    for(uint32_t i=0;i<info->layerCount;++i)if(info->layers[i]->type==XR_TYPE_COMPOSITION_LAYER_PROJECTION)
    {
        ++runtime.projection_layers;
        const auto* projection=reinterpret_cast<const XrCompositionLayerProjection*>(info->layers[i]);
        for(uint32_t v=0;v<projection->viewCount;++v)
            if(const auto* depth=reinterpret_cast<const XrCompositionLayerDepthInfoKHR*>(projection->views[v].next))
            {
                REQUIRE(depth->type==XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR&&depth->subImage.swapchain);
                REQUIRE(depth->nearZ>0&&depth->farZ>depth->nearZ&&depth->maxDepth==1.f);
                ++runtime.depth_views;
            }
    }
    for(uint32_t i=0;i<info->layerCount;++i) if(info->layers[i]->type==XR_TYPE_COMPOSITION_LAYER_QUAD)
    {
        const auto* quad=reinterpret_cast<const XrCompositionLayerQuad*>(info->layers[i]);
        runtime.quad_space=quad->space;runtime.quad_pose=quad->pose;runtime.quad_width=quad->size.width;
    }
    for(auto*c:chains)REQUIRE(!c->acquired || !c->waited);return XR_SUCCESS;
}
static XrResult XRAPI_PTR LocateViews(XrSession,const XrViewLocateInfo*,XrViewState*state,uint32_t,uint32_t*count,XrView*out)
{
    *count=2;state->viewStateFlags=runtime.tracking?XR_VIEW_STATE_POSITION_VALID_BIT|XR_VIEW_STATE_ORIENTATION_VALID_BIT:0;
    for(int i=0;i<2;++i){out[i].pose={{0,std::sin(runtime.head_yaw*0.5f),0,std::cos(runtime.head_yaw*0.5f)},{runtime.head_x+(i==0?-0.031f:0.031f),runtime.head_y,0}};out[i].fov={-0.7f,0.8f,0.9f,-0.75f};}return XR_SUCCESS;
}
static XrResult XRAPI_PTR Formats(XrSession,uint32_t capacity,uint32_t*count,int64_t*out)
{
    // Deliberately advertise linear UNORM first: gamma-encoded engine output
    // must still select sRGB for compositor interpretation.
    std::vector<int64_t> formats{DXGI_FORMAT_R8G8B8A8_UNORM};
    if(runtime.color_format!=DXGI_FORMAT_R8G8B8A8_UNORM)formats.push_back(runtime.color_format);
    if(runtime.depth)formats.push_back(DXGI_FORMAT_D24_UNORM_S8_UINT);
    *count=uint32_t(formats.size());
    for(uint32_t i=0;i<capacity&&i<*count;++i)out[i]=formats[i];
    return XR_SUCCESS;
}
static XrResult XRAPI_PTR Views(XrInstance,XrSystemId,XrViewConfigurationType,uint32_t capacity,uint32_t*count,XrViewConfigurationView*out)
{
    *count=2;if(capacity)for(int i=0;i<2;++i){out[i].recommendedImageRectWidth=128+i*16;out[i].recommendedImageRectHeight=160;
    out[i].maxImageRectWidth=out[i].maxImageRectHeight=512;out[i].recommendedSwapchainSampleCount=4;}return XR_SUCCESS;
}
static XrResult XRAPI_PTR CreateChain(XrSession,const XrSwapchainCreateInfo*info,XrSwapchain*out)
{
    if(runtime.fail_chain && runtime.chains+1==runtime.fail_chain)return XR_ERROR_RUNTIME_FAILURE;
    REQUIRE(info->sampleCount==1);auto*c=new Chain;
    D3D11_TEXTURE2D_DESC d{};d.Width=info->width;d.Height=info->height;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
    if(info->format==DXGI_FORMAT_D24_UNORM_S8_UINT)
    {
        REQUIRE(runtime.depth&&(info->usageFlags&XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT));
        d.Format=DXGI_FORMAT_R24G8_TYPELESS;d.BindFlags=D3D11_BIND_DEPTH_STENCIL;
    }
    else
    {
        REQUIRE(info->format==runtime.color_format);
        d.Format=info->format==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB?DXGI_FORMAT_B8G8R8A8_TYPELESS:DXGI_FORMAT_R8G8B8A8_TYPELESS;
        d.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
    }
    REQUIRE(SUCCEEDED(runtime.device->CreateTexture2D(&d,nullptr,&c->texture)));
    chains.push_back(c);++runtime.chains;*out=reinterpret_cast<XrSwapchain>(c);return XR_SUCCESS;
}
static XrResult XRAPI_PTR Images(XrSwapchain handle,uint32_t capacity,uint32_t*count,XrSwapchainImageBaseHeader*out)
{*count=1;if(capacity)reinterpret_cast<XrSwapchainImageD3D11KHR*>(out)->texture=reinterpret_cast<Chain*>(handle)->texture;return XR_SUCCESS;}
static XrResult XRAPI_PTR DestroyChain(XrSwapchain handle)
{auto*c=reinterpret_cast<Chain*>(handle);c->texture->Release();chains.erase(std::find(chains.begin(),chains.end(),c));delete c;--runtime.chains;return XR_SUCCESS;}
static XrResult XRAPI_PTR Acquire(XrSwapchain handle,const XrSwapchainImageAcquireInfo*,uint32_t*out)
{auto*c=reinterpret_cast<Chain*>(handle);REQUIRE(!c->acquired);c->acquired=true;c->waited=false;*out=0;++runtime.acquires;return XR_SUCCESS;}
static XrResult XRAPI_PTR WaitImage(XrSwapchain handle,const XrSwapchainImageWaitInfo*)
{auto*c=reinterpret_cast<Chain*>(handle);REQUIRE(c->acquired);if(runtime.wait_image==XR_SUCCESS)c->waited=true;return runtime.wait_image;}
static XrResult XRAPI_PTR Release(XrSwapchain handle,const XrSwapchainImageReleaseInfo*)
{auto*c=reinterpret_cast<Chain*>(handle);REQUIRE(c->acquired&&c->waited);c->acquired=c->waited=false;++runtime.releases;return XR_SUCCESS;}
static XrResult XRAPI_PTR CreateSet(XrInstance,const XrActionSetCreateInfo*,XrActionSet*out)
{++runtime.sets;*out=reinterpret_cast<XrActionSet>(3);return XR_SUCCESS;}
static XrResult XRAPI_PTR DestroySet(XrActionSet){--runtime.sets;runtime.actions=0;return XR_SUCCESS;}
static std::vector<std::string> input_paths, input_action_names(1);
static XrResult XRAPI_PTR Path(XrInstance,const char* name,XrPath*out)
{
    auto found=std::find(input_paths.begin(),input_paths.end(),name);
    if(found==input_paths.end()){input_paths.push_back(name);*out=input_paths.size();}
    else *out=1+std::distance(input_paths.begin(),found);
    return XR_SUCCESS;
}
static XrResult XRAPI_PTR Action(XrActionSet,const XrActionCreateInfo* info,XrAction*out)
{
    if(runtime.fail_action&&runtime.actions+1==runtime.fail_action)return XR_ERROR_RUNTIME_FAILURE;
    *out=reinterpret_cast<XrAction>(uintptr_t(100+ ++runtime.actions));
    input_action_names.resize(runtime.actions+1);input_action_names[runtime.actions]=info->actionName;
    return XR_SUCCESS;
}
static XrResult XRAPI_PTR ActionSpace(XrSession,const XrActionSpaceCreateInfo*,XrSpace*out)
{*out=reinterpret_cast<XrSpace>(uintptr_t(20+ ++runtime.spaces));return XR_SUCCESS;}
static XrResult XRAPI_PTR Bindings(XrInstance,const XrInteractionProfileSuggestedBinding*info)
{
    // Touch, Index, Mixed Reality, simple, Vive (the HP profile needs its
    // extension, which the mock runtime does not offer).
    const int profile=runtime.binding_profiles++%5;
    const unsigned expected[]={21u,21u,21u,8u,20u};
    REQUIRE(info->countSuggestedBindings==expected[profile]);
    bool right_button=false, left_menu=false, left_sprint=false, right_jump=false;
    int pad_move=0, pad_button=0, secondary=0;
    // Touch and Index click the sticks; Mixed Reality's stick clicks are its
    // face buttons, so the lower trackpad half sprints and jumps and the
    // upper half is the secondary button. Index has no menu button. Vive's
    // trackpad is its stick and its right menu button jumps.
    const char* menu=profile==1 ? "/user/hand/left/input/trackpad/force" : "/user/hand/left/input/menu/click";
    for(unsigned i=0;i<info->countSuggestedBindings;++i)
    {
        const auto& binding=info->suggestedBindings[i];
        const auto& action=input_action_names.at(uintptr_t(binding.action)-100);
        const auto& path=input_paths.at(binding.binding-1);
        if(action=="menu_left")
        {
            REQUIRE(path==menu);
            left_menu=true;
        }
        if(action=="sprint_left")
        {
            REQUIRE(path=="/user/hand/left/input/thumbstick/click");
            left_sprint=true;
        }
        if(action=="jump_right")
        {
            REQUIRE(path==(profile==4 ? "/user/hand/right/input/menu/click" : "/user/hand/right/input/thumbstick/click"));
            right_jump=true;
        }
        if(action.rfind("pad_move",0)==0){REQUIRE(path.find("/input/trackpad/click")!=std::string::npos);++pad_move;}
        if(action.rfind("pad_button",0)==0){REQUIRE(path.find("/input/trackpad/click")!=std::string::npos);++pad_button;}
        if(action.rfind("secondary",0)==0)++secondary;
        if(action=="thumbstick_left" && profile==4)REQUIRE(path=="/user/hand/left/input/trackpad");
        if(action!="button_right")continue;
        right_button=true;
        REQUIRE(path==(profile==2 ?
            "/user/hand/right/input/thumbstick/click" : "/user/hand/right/input/a/click"));
    }
    REQUIRE(right_button==(profile<=2));
    REQUIRE(left_menu==(profile!=3));
    REQUIRE(left_sprint==(profile<=1) && right_jump==(profile<=1 || profile==4));
    REQUIRE(pad_move==(profile==2?2:0) && pad_button==(profile==4?2:0));
    REQUIRE(secondary==(profile<=1?2:0));
    return XR_SUCCESS;
}
static XrResult XRAPI_PTR Attach(XrSession,const XrSessionActionSetsAttachInfo*){runtime.attached=true;return XR_SUCCESS;}
static XrResult XRAPI_PTR Sync(XrSession,const XrActionsSyncInfo*){REQUIRE(runtime.attached&&runtime.running);++runtime.syncs;return runtime.active?XR_SUCCESS:XR_SESSION_NOT_FOCUSED;}
static XrResult XRAPI_PTR Boolean(XrSession,const XrActionStateGetInfo*,XrActionStateBoolean*out)
{out->isActive=runtime.active;out->currentState=XR_TRUE;return XR_SUCCESS;}
static XrResult XRAPI_PTR Float(XrSession,const XrActionStateGetInfo*,XrActionStateFloat*out){out->isActive=runtime.active;out->currentState=0.75f;return XR_SUCCESS;}
static XrResult XRAPI_PTR Vector(XrSession,const XrActionStateGetInfo*,XrActionStateVector2f*out){out->isActive=runtime.active;out->currentState={0.5f,0.25f};return XR_SUCCESS;}
static XrResult XRAPI_PTR Pose(XrSession,const XrActionStateGetInfo*,XrActionStatePose*out){out->isActive=runtime.active;return XR_SUCCESS;}
static XrResult XRAPI_PTR Locate(XrSpace,XrSpace,XrTime,XrSpaceLocation*out)
{out->locationFlags=runtime.tracking?XR_SPACE_LOCATION_POSITION_VALID_BIT|XR_SPACE_LOCATION_ORIENTATION_VALID_BIT:0;out->pose={{0,std::sin(runtime.head_yaw*0.5f),0,std::cos(runtime.head_yaw*0.5f)},{runtime.head_x,runtime.head_y,0}};return XR_SUCCESS;}
static XrResult XRAPI_PTR Haptic(XrSession,const XrHapticActionInfo*,const XrHapticBaseHeader*){return XR_SUCCESS;}
namespace vr
{
const char*ResultToString(XrResult){return "mock result";}
bool Succeeded(XrResult result,const char*){return XR_SUCCEEDED(result);}
bool LoadOpenXR(){return runtime.loader;}
bool LoadOpenXRInstance(XrInstance){return true;}
void UnloadOpenXR(){}
}
static void Connect()
{
    xrEnumerateInstanceExtensionProperties_p=EnumerateExtensions;xrCreateInstance_p=CreateInstance;
    xrDestroyInstance_p=DestroyInstance;xrGetInstanceProperties_p=Properties;xrGetSystem_p=GetSystem;xrGetInstanceProcAddr_p=Proc;
    xrCreateSession_p=CreateSession;xrDestroySession_p=DestroySession;xrCreateReferenceSpace_p=CreateSpace;xrDestroySpace_p=DestroySpace;
    xrBeginSession_p=BeginSession;xrEndSession_p=EndSession;xrPollEvent_p=Poll;xrWaitFrame_p=WaitFrame;xrBeginFrame_p=BeginFrame;xrEndFrame_p=EndFrame;
    xrLocateViews_p=LocateViews;xrEnumerateSwapchainFormats_p=Formats;xrEnumerateViewConfigurationViews_p=Views;xrCreateSwapchain_p=CreateChain;
    xrEnumerateSwapchainImages_p=Images;xrDestroySwapchain_p=DestroyChain;xrAcquireSwapchainImage_p=Acquire;xrWaitSwapchainImage_p=WaitImage;xrReleaseSwapchainImage_p=Release;
    xrCreateActionSet_p=CreateSet;xrDestroyActionSet_p=DestroySet;xrStringToPath_p=Path;xrCreateAction_p=Action;xrCreateActionSpace_p=ActionSpace;
    xrSuggestInteractionProfileBindings_p=Bindings;xrAttachSessionActionSets_p=Attach;xrSyncActions_p=Sync;
    xrGetActionStateBoolean_p=Boolean;xrGetActionStateFloat_p=Float;xrGetActionStateVector2f_p=Vector;xrGetActionStatePose_p=Pose;xrLocateSpace_p=Locate;xrApplyHapticFeedback_p=Haptic;
}
static bool Close(float a,float b){return std::abs(a-b)<0.0001f;}
static void Math()
{
    REQUIRE(vr::LaunchOverride(nullptr)==-1);REQUIRE(vr::LaunchOverride("-VR")==2);
    REQUIRE(vr::LaunchOverride("-vr -NoVr")==0);REQUIRE(vr::LaunchOverride("-vrdebug file-vr -novr-extra")==-1);
    REQUIRE(vr::LaunchOverride("\"-vr\"")==2);REQUIRE(vr::LaunchOverride("\"folder -vr\"")==-1);
    REQUIRE(vr::LaunchOverride("\"-vr\"suffix")==-1);
    REQUIRE(vr::LaunchOverride("-v\"r\"")==2);
    REQUIRE(vr::LaunchOverride("\"-novr\"suffix -vr")==2);
    const auto p=vr::ToEnginePosition({1,2,-3});REQUIRE(p.x==1&&p.y==2&&p.z==3);
    Fvector r,u,f;vr::ToEngineBasis({0,0,0,1},r,u,f);REQUIRE(r.x==1&&u.y==1&&f.z==1);
    vr::ToEngineBasis({0,0.70710678f,0,0.70710678f},r,u,f);REQUIRE(Close(f.x,-1)&&Close(f.z,0));
    const XrFovf fov{-0.6f,0.9f,0.8f,-0.7f};
    const auto matrix=vr::MakeAsymmetricProjection(fov,0.2f,1000);
    REQUIRE(matrix._13==0&&matrix._23==0);
    REQUIRE(Close(tanf(fov.angleLeft)*matrix._11+matrix._31,-1));
    REQUIRE(Close(tanf(fov.angleRight)*matrix._11+matrix._31,1));
    REQUIRE(Close(tanf(fov.angleDown)*matrix._22+matrix._32,-1));
    REQUIRE(Close(tanf(fov.angleUp)*matrix._22+matrix._32,1));
    REQUIRE(Close((0.2f*matrix._33+matrix._43)/0.2f,0));
    REQUIRE(Close((1000*matrix._33+matrix._43)/1000,1));
}
static void CheckColorView(ID3D11RenderTargetView* target)
{
    D3D11_RENDER_TARGET_VIEW_DESC view{};target->GetDesc(&view);
    const auto expected=runtime.color_format==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB?DXGI_FORMAT_B8G8R8A8_UNORM:DXGI_FORMAT_R8G8B8A8_UNORM;
    REQUIRE(view.Format==expected);
    ID3D11DeviceContext* context=nullptr;runtime.device->GetImmediateContext(&context);
    const float encoded[4]={0.25f,0.5f,0.75f,1};
    context->ClearRenderTargetView(target,encoded);
    ID3D11Resource* resource=nullptr;target->GetResource(&resource);
    ID3D11Texture2D* texture=nullptr;
    REQUIRE(SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D),reinterpret_cast<void**>(&texture))));
    D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);
    desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging=nullptr;
    REQUIRE(SUCCEEDED(runtime.device->CreateTexture2D(&desc,nullptr,&staging)));
    context->CopyResource(staging,texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    REQUIRE(SUCCEEDED(context->Map(staging,0,D3D11_MAP_READ,0,&mapped)));
    const auto* bytes=static_cast<const unsigned char*>(mapped.pData);
    // The engine's encoded midpoint must stay 128, not become 188 through
    // an additional hardware sRGB encode. Check channels too (RGBA/BGRA).
    REQUIRE(bytes[1]>=127&&bytes[1]<=128&&bytes[3]==255);
    REQUIRE(bytes[expected==DXGI_FORMAT_B8G8R8A8_UNORM?2:0]==64);
    REQUIRE(bytes[expected==DXGI_FORMAT_B8G8R8A8_UNORM?0:2]==191);
    context->Unmap(staging,0);staging->Release();texture->Release();resource->Release();context->Release();
}
struct HandDrawer
{
    struct Line { Fvector start, end; };
    std::vector<std::array<Fvector,3>> faces;
    std::vector<Line> lines;
    // Deliberately expose only DX11-supported dynamic primitive operations.
    void DrawFace(const Fvector& a,const Fvector& b,const Fvector& c,unsigned,unsigned,bool solid,bool wire)
    { REQUIRE(solid&&!wire);faces.push_back({a,b,c}); }
    void DrawLine(const Fvector& a,const Fvector& b,unsigned) { lines.push_back({a,b}); }
};
static void HandRendering()
{
    Fmatrix world;world.identity();world.c.set(1,2,3);
    world.i.set(0,0,-1);world.k.set(1,0,0);
    HandDrawer open,closed,right;
    vr::DrawHandProxy(open,world,0,0,0xffffffff);
    vr::DrawHandProxy(closed,world,0,1,0xffffffff);
    vr::DrawHandProxy(right,world,1,1,0xffffffff);
    for (const auto* drawer : {&open,&closed,&right})
    {
        REQUIRE(drawer->faces.size()==12&&drawer->lines.size()==21);
        for (const auto& face : drawer->faces)
        {
            Fvector edge0{face[1].x-face[0].x,face[1].y-face[0].y,face[1].z-face[0].z};
            Fvector edge1{face[2].x-face[0].x,face[2].y-face[0].y,face[2].z-face[0].z};
            Fvector normal;normal.crossproduct(edge0,edge1);
            REQUIRE(normal.square_magnitude()>0);
            // Every triangle faces outward from the palm centre.
            REQUIRE(normal.x*(face[0].x-world.c.x)+normal.y*(face[0].y-world.c.y)+normal.z*(face[0].z-world.c.z)>0);
            for(const auto& point:face)
            {
                REQUIRE(point.x>=0.9549f&&point.x<=1.0451f);
                REQUIRE(point.y>=1.9819f&&point.y<=2.0181f);
                REQUIRE(point.z>=2.9649f&&point.z<=3.0351f);
            }
        }
        for (const auto& line : drawer->lines)
            REQUIRE(std::isfinite(line.start.x)&&std::isfinite(line.end.x)&&std::isfinite(line.end.y)&&std::isfinite(line.end.z));
    }
    REQUIRE(Close(open.lines[13].end.x,1.1f)&&Close(closed.lines[13].end.x,1.05f));
    REQUIRE(Close(closed.lines[13].end.y,1.945f));
    REQUIRE(Close(open.lines.back().end.z,2.955f)&&Close(right.lines.back().end.z,3.045f));
}
static void MenuPointerPolicy()
{
    vr::UiConsumedControls consumed;
    consumed.Consume(2|4);
    REQUIRE(consumed.Filter(2|4)==0&&consumed.Blocked(2));
    REQUIRE(consumed.Filter(2|1)==1&&!consumed.Blocked(4));
    REQUIRE(consumed.Filter(0)==0&&!consumed.Blocked(2));
    REQUIRE(consumed.Filter(2|4)==(2|4));
    vr::UiConfirmState confirm;
    REQUIRE(!confirm.Step(false,0));
    REQUIRE(!confirm.Step(true,2,7)); // held grip on entry cannot dismiss
    REQUIRE(confirm.Step(true,2|4,7)); // a fresh A works despite that held grip
    REQUIRE(!confirm.Step(true,2|4,7));
    REQUIRE(!confirm.Step(true,2,7));REQUIRE(confirm.Step(true,2|1,7));
    REQUIRE(!confirm.Step(false,2|1,7));REQUIRE(!confirm.Step(true,2|1,7));
    REQUIRE(!confirm.Step(true,0,7));REQUIRE(confirm.Step(true,1,7));
    REQUIRE(!confirm.Step(true,0,0));REQUIRE(!confirm.Step(true,1,7)); // returning held controller
    REQUIRE(!confirm.Step(true,0,7));REQUIRE(confirm.Step(true,4,7));

    float u,v;
    bool inside=false;
    // On the panel: inside, at the point the ray meets it.
    const auto on_panel=[&](const Fvector& origin, const Fvector& direction, float distance, float width, float height)
    { return vr::QuadRayCursor(origin,direction,distance,width,height,u,v,inside) && inside; };
    REQUIRE(on_panel({0,0,0},{0,0,1},1.5f,1.5f,1)&&Close(u,0.5f)&&Close(v,0.5f));
    REQUIRE(on_panel({0,0,0},{0.5f,0.333333333f,1},1.5f,1.5f,1)&&Close(u,1)&&Close(v,0));
    REQUIRE(!on_panel({0,0,0},{1,0,1},1.5f,1.5f,1));
    REQUIRE(!on_panel({0,0,0},{0,1,0},1.5f,1.5f,1));
    REQUIRE(!on_panel({0,0,0},{0,0,-1},1.5f,1.5f,1));
    REQUIRE(!on_panel({0,0,2},{0,0,1},1.5f,1.5f,1));
    REQUIRE(on_panel({0.2f,0.1f,0.4f},{0,0,1},3,3,2));
    REQUIRE(Close(u,0.5f+0.2f/3)&&Close(v,0.45f));
    REQUIRE(vr::QuadRayCursor({0,0,0},{0,0,1},1.5f,1.5f,1,u,v,inside));
    REQUIRE(inside&&Close(u,0.5f)&&Close(v,0.5f));
    REQUIRE(vr::QuadRayCursor({0,0,0},{1,0,1},1.5f,1.5f,1,u,v,inside));
    REQUIRE(!inside&&Close(u,1.f)&&Close(v,0.5f));
    REQUIRE(!vr::QuadRayCursor({0,0,0},{0,0,-1},1.5f,1.5f,1,u,v,inside));
    REQUIRE(!inside&&Close(u,-1.f)&&Close(v,-1.f));
    vr::UiClickState click;
    REQUIRE(click.Step(true,true)==0); // held trigger on menu entry is not a click
    REQUIRE(click.Step(true,false)==0);
    REQUIRE(click.Step(true,true)==1&&click.Step(true,true)==0);
    REQUIRE(click.Step(false,true)==-1&&click.Step(false,true)==0); // cancel once on loss
    REQUIRE(click.Step(true,true)==0);
    REQUIRE(click.Step(true,false)==0&&click.Step(true,true)==1);
    REQUIRE(click.Step(true,false)==-1&&click.Step(true,false)==0);
    REQUIRE(!vr::CancelUiClickOnPointerChange(false,true,false));
    REQUIRE(vr::CancelUiClickOnPointerChange(false,true,true));
    REQUIRE(vr::CancelUiClickOnPointerChange(true,false,false));
    vr::UiClickState switched;
    REQUIRE(switched.Step(true,false)==0);
    // Selecting the pressed controller must keep this prior release armed.
    REQUIRE(!vr::CancelUiClickOnPointerChange(false,true,switched.held));
    REQUIRE(switched.Step(true,true)==1);
    // Stick as a wheel: a step at once, then repeating faster when pushed further.
    vr::UiScrollRepeat wheel;
    REQUIRE(wheel.Step(0.2f,0.016f)==0);
    REQUIRE(wheel.Step(0.5f,0.016f)==1);
    int half=0, full=0;
    for (int i=0;i<60;++i) half+=wheel.Step(0.5f,1.f/60.f);
    REQUIRE(wheel.Step(0.f,0.016f)==0 && wheel.Step(-1.f,0.016f)==-1);
    for (int i=0;i<60;++i) full-=wheel.Step(-1.f,1.f/60.f);
    REQUIRE(half>=5 && half<=7 && full>=11 && full<=13);
    // The HUD grown k times keeps each element's anchor on screen: corners,
    // edges and the centre.
    const auto shown=[](float p, float k, float screen){ return 0.5f*screen+(p-0.5f*screen)*k; };
    const auto Near=[](float x, float y){ return std::abs(x-y)<0.01f; };
    float dx=0, dy=0;
    vr::HudInsetOffset(824.f,668.f,200.f,100.f,1024.f,768.f,1.5f,dx,dy);   // bottom right
    REQUIRE(Near(shown(1024.f+dx,1.5f,1024.f),1024.f) && Near(shown(768.f+dy,1.5f,768.f),768.f));
    vr::HudInsetOffset(0.f,0.f,200.f,100.f,1024.f,768.f,1.5f,dx,dy);       // top left
    REQUIRE(Near(shown(0.f+dx,1.5f,1024.f),0.f) && Near(shown(0.f+dy,1.5f,768.f),0.f));
    vr::HudInsetOffset(412.f,0.f,200.f,100.f,1024.f,768.f,1.5f,dx,dy);     // top centre
    REQUIRE(Near(dx,0.f) && Near(shown(0.f+dy,1.5f,768.f),0.f));
    vr::HudInsetOffset(824.f,668.f,200.f,100.f,1024.f,768.f,0.85f,dx,dy);  // smaller: unchanged
    REQUIRE(dx==0.f && dy==0.f);
}

struct ProbeBox
{
    Fvector size{0.1f,0.2f,0.3f}; Fmatrix form;
    ProbeBox(){form.identity();form.c.set(1,2,3);}
    void get_size(Fvector& out)const{out=size;}
    void set_size(const Fvector& value){size=value;}
    void get_local_form_bt(Fmatrix& out)const{out=form;}
    void set_local_form_bt(const Fmatrix& value){form=value;}
};
struct ProbeCylinder
{
    float r=0.5f,h=0.75f; Fmatrix form;
    ProbeCylinder(){form.identity();form.c.set(4,5,6);}
    float radius()const{return r;} float height()const{return h;}
    void set_radius(float value){r=value;} void set_height(float value){h=value;}
    void get_local_form_bt(Fmatrix& out)const{out=form;}
    void set_local_form_bt(const Fmatrix& value){form=value;}
};
struct ProbeElement
{
    Fmatrix form; int history=-1;
    ProbeElement(){form.identity();form.c.set(7,8,9);}
    void GetGlobalTransformDynamic(Fmatrix* out)const{*out=form;}
    void SetTransform(const Fmatrix& value,int state){form=value;history=state;}
};
static void PhysicsProbeAndFrameWork()
{
    ProbeBox box;ProbeCylinder cylinder;ProbeElement element;
    const Fvector saved_size=box.size;
    const Fmatrix saved_box=box.form,saved_cylinder=cylinder.form,saved_element=element.form;
    for(int query=0;query<10000;++query)
    {
        {
            ScopedCameraProbeState restore(box,cylinder,element,17);
            box.set_size({0.12f,0.12f,0.12f});box.form.identity();
            cylinder.set_radius(0.12f);cylinder.set_height(0.24f);cylinder.form.identity();
            element.form.identity();element.form.c.set(float(query),0,0);
        }
        REQUIRE(Close(box.size.x,saved_size.x)&&Close(box.size.y,saved_size.y)&&Close(box.size.z,saved_size.z));
        REQUIRE(cylinder.r==0.5f&&cylinder.h==0.75f&&element.history==17);
        REQUIRE(std::memcmp(&box.form,&saved_box,sizeof(Fmatrix))==0);
        REQUIRE(std::memcmp(&cylinder.form,&saved_cylinder,sizeof(Fmatrix))==0);
        REQUIRE(std::memcmp(&element.form,&saved_element,sizeof(Fmatrix))==0);
    }
    struct Queue:std::vector<std::function<void()>>{void clear_not_free(){clear();}} work;
    std::vector<int> order;int physics_steps=0;
    work.push_back([&]{order.push_back(1);});work.push_back([&]{order.push_back(2);});
    vr::RunFrameWork(work,[&]{++physics_steps;order.push_back(3);});
    REQUIRE(work.empty()&&physics_steps==1);
    for(int eye=0;eye<2;++eye){REQUIRE(physics_steps==1);order.push_back(4+eye);}
    REQUIRE(order==std::vector<int>({1,2,3,4,5}));
}

static void GameplayPolicies()
{
    float x=0.1f,y=0.1f;vr::FilterStick(x,y,0.2f);REQUIRE(x==0&&y==0);
    x=1;y=1;vr::FilterStick(x,y,0.2f);REQUIRE(Close(std::sqrt(x*x+y*y),1));
    x=0.6f;y=0;vr::FilterStick(x,y,0.2f);REQUIRE(Close(x,0.5f));
    vr::TurnState turning;
    REQUIRE(turning.Step(1,0.1f,true,false,30,90)==0); // no turn on reactivation while held
    turning.Step(0,0.1f,true,false,30,90);
    REQUIRE(turning.Step(0.8f,0.1f,true,false,30,90)==30);
    REQUIRE(turning.Step(1,0.1f,true,false,30,90)==0);
    turning.Step(0.2f,0.1f,true,false,30,90);
    REQUIRE(turning.Step(-0.8f,0.1f,true,false,30,90)==-30);
    turning.Reset();REQUIRE(turning.Step(1,5,true,true,30,90)==9); // bounded timestep, smooth takes precedence
    REQUIRE(vr::HeadSweepFraction(1,[](float){return false;})==1);
    const float safe=vr::HeadSweepFraction(1,[](float t){return t>=0.437f;});
    REQUIRE(safe<0.437f&&safe>0.436f);
    REQUIRE(vr::HeadSweepFraction(1,[](float t){return t<0.1f;})==0);
    // Thin obstruction along the path must block even when the endpoint is free.
    REQUIRE(vr::HeadSweepFraction(1,[](float t){return t>=0.4f&&t<=0.43f;})<0.4f);
    // Shared shadow frustum: the union of the eyes' corners in the head's
    // frame. Two eyes canted 10 degrees outward widen it by 10 each side.
    vr::FovAngles fov;
    REQUIRE(fov.empty);
    const float cant=0.1745329f, l=-0.7f, r=0.8f, u=0.9f, d=-0.75f;
    for (int eye=0;eye<2;++eye)
    {
        const float yaw=eye==0?-cant:cant;
        for (int corner=0;corner<4;++corner)
        {
            const float x=std::tan(corner&1?r:l), y=std::tan(corner&2?u:d);
            fov.Include(x*std::cos(yaw)+std::sin(yaw),y,-x*std::sin(yaw)+std::cos(yaw));
        }
    }
    REQUIRE(!fov.empty&&Close(fov.left,l-cant)&&Close(fov.right,r+cant));
    REQUIRE(fov.up>=u-1e-4f&&fov.down<=d+1e-4f);
    fov.Include(1,0,-1); // behind the frame: ignored
    REQUIRE(Close(fov.right,r+cant));
    // Scope view size: the lens' footprint, bucketed, eye size beyond.
    REQUIRE(vr::ScopeResolution(8,100,2000,1,0)==256);        // ~117 px
    REQUIRE(vr::ScopeResolution(8,100,2000,0,0)==2000);       // quality 0: eye size
    REQUIRE(vr::ScopeResolution(0,100,2000,1,0)==2000);       // no lens size given
    REQUIRE(vr::ScopeResolution(30,100,2000,1,0)==512);       // ~450 px
    REQUIRE(vr::ScopeResolution(40,100,2000,1,0)==768);       // ~611 px
    REQUIRE(vr::ScopeResolution(30,100,2000,2,0)==2000);      // beyond the largest bucket
    REQUIRE(vr::ScopeResolution(20,100,200,1,0)==200);        // capped at the eye
    // Hysteresis: kept while the need fits and is not 15% below the bucket under.
    REQUIRE(vr::ScopeResolution(20,100,2000,1,0)==384);       // ~306 px
    REQUIRE(vr::ScopeResolution(16.5f,100,2000,1,384)==384);  // ~245 px > 0.85*256
    REQUIRE(vr::ScopeResolution(13,100,2000,1,384)==256);     // ~191 px
    REQUIRE(vr::ScopeResolution(21,100,2000,1,256)==384);     // outgrows 256
}
static bool HeightEye(const VrEyeRenderContext& eye,void*)
{
    Fmatrix world;world.invert(eye.view);
    REQUIRE(Close(world.c.y,1.65f));
    REQUIRE(Close(world.c.x,eye.eye==0?-0.031f:0.031f));
    // The shared shadow camera sits between the eyes; with matching eyes its
    // frustum is theirs.
    Fmatrix shadow;shadow.invert(eye.shadow_view);
    REQUIRE(Close(shadow.c.x,0)&&Close(shadow.c.y,1.65f));
    REQUIRE(Close(eye.shadow_projection._11,eye.projection._11)&&Close(eye.shadow_projection._22,eye.projection._22));
    REQUIRE(Close(eye.shadow_projection._31,eye.projection._31)&&Close(eye.shadow_projection._32,eye.projection._32));
    return true;
}
struct PistolTestBone
{
    Fmatrix mTransform, mTransformHidden, mRenderTransform;
    void (*callback)(PistolTestBone*)=nullptr; void* parameter=nullptr;
    void* callback_param() {return parameter;}
    void set_callback(unsigned,void (*function)(PistolTestBone*),void* value,bool overwrite)
    {REQUIRE(overwrite);callback=function;parameter=value;}
};
struct PistolTestData
{
    unsigned short parent=0;
    unsigned short GetParentID() const {return parent;}
};
struct PistolTestModel
{
    static constexpr int bone_count=13;
    const char* names[bone_count]={"wpn_body","bullets","gilza","hammer","magazin","muzzle","siodelko","slide","sprezyna","trigger","wpn_silencer","magazin_2","reload_rounds"};
    std::vector<Fmatrix> bind;
    PistolTestBone bones[bone_count]; bool visible[bone_count]; PistolTestData data[bone_count];
    bool invalid=true;
    PistolTestModel()
    {
        bind.resize(bone_count);
        for(int i=0;i<bone_count;++i){bind[i].identity();bones[i].mTransformHidden.identity();visible[i]=true;}
        bind[7].c.set(0,0.0355531f,0.0659581f);bind[4].c.set(0,-0.0237147f,-0.0004783f);
        // wpn_body is the root; gilza stands in for an unlisted magazine sub-bone.
        data[0].parent=65535;data[2].parent=4;data[12].parent=11;
    }
    PistolTestData& LL_GetData(unsigned short i){return data[i];}
    unsigned short LL_BoneID(const char* name){for(unsigned short i=0;i<bone_count;++i)if(std::strcmp(name,names[i])==0)return i;return 65535;}
    unsigned short LL_BoneCount(){return bone_count;}
    bool LL_GetBoneVisible(unsigned short i){return visible[i];}
    void LL_SetBoneVisible(unsigned short i,bool value,bool)
    {visible[i]=value;bones[i].mTransform=bones[i].mTransformHidden;invalid=true;}
    PistolTestBone& LL_GetBoneInstance(unsigned short i){return bones[i];}
    void LL_GetBindTransform(std::vector<Fmatrix>& result){result=bind;}
    void CalculateBones_Invalidate(){invalid=true;}
    void CalculateBones(bool)
    {
        if(!invalid)return;
        for(int i=0;i<bone_count;++i)
        {
            auto& bone=bones[i];
            // A renderer recalculation can otherwise discard a one-off pose.
            bone.mTransform.identity();
            if(bone.callback)bone.callback(&bone);
            Fmatrix inverse;inverse.invert(bind[i]);bone.mRenderTransform.mul_43(bone.mTransform,inverse);
        }
        invalid=false;
    }
};
static void PistolTestCallback(PistolTestBone* bone)
{bone->mTransform=*static_cast<Fmatrix*>(bone->callback_param());}
static void ReloadMagazineRegression()
{
    Fmatrix parent; parent.identity();
    const Fmatrix spare=ak12_fixture::Pose<Fmatrix>(ak12_fixture::reload_rotation);
    const Fmatrix canonical=ak12_fixture::Pose<Fmatrix>(ak12_fixture::canonical_rotation);
    constexpr float degrees=180.f/3.14159265359f;
    // The old bind-relative comparison sees a large turn in a static key.
    REQUIRE(std::abs(vr::TurnAbout(parent,spare,parent,0)*degrees-161.66049f)<0.01f);
    REQUIRE(std::abs(vr::TurnAbout(parent,canonical,parent,0)*degrees-177.78519f)<0.01f);
    for (int axis:{0,2})
    {
        REQUIRE(vr::TurnAbout(spare,spare,parent,axis)<0.001f);
        REQUIRE(vr::TurnAbout(canonical,canonical,parent,axis)<0.001f);
    }
    // Actual action rotation is still measured, even with a rotated parent.
    Fmatrix lever; lever.identity();
    lever.j.set(0,0,1); lever.k.set(0,-1,0);
    REQUIRE(Close(vr::TurnAbout(parent,lever,parent,0)*degrees,90.f));
    Fmatrix bolt; bolt.identity(); bolt.i.set(0,1,0); bolt.j.set(-1,0,0);
    REQUIRE(Close(vr::TurnAbout(parent,bolt,parent,2)*degrees,90.f));
    REQUIRE(Close(vr::TurnAbout(parent,bolt,bolt,2)*degrees,90.f));

    // Synthetic ownership tree: receiver, canonical magazine with child,
    // sibling reload copy with child. The numeric pose above is real stock
    // evidence; this small tree deliberately exercises conflicting owners.
    PistolTestModel weapon, loose;
    weapon.bind[11].c.set(ak12_fixture::reload_bind_position[0],
        ak12_fixture::reload_bind_position[1],ak12_fixture::reload_bind_position[2]);
    loose.bind=weapon.bind;
    std::vector<Fmatrix> weapon_poses, loose_poses;
    const std::vector<const char*> reload_roots={"magazin_2","missing_on_dedicated_asset"};
    for (bool inserted:{true,false,true,false})
    {
        vr::ApplyWeaponPose(weapon,weapon_poses,PistolTestCallback,2,nullptr,0.f,"bullets",
            {"magazin","reload_rounds"},false,inserted,inserted,nullptr,{}, {},nullptr,0.f,reload_roots);
        vr::ApplyWeaponPose(loose,loose_poses,PistolTestCallback,2,nullptr,0.f,"bullets",
            {"magazin","reload_rounds"},true,false,true,nullptr,{}, {},nullptr,0.f,reload_roots);
        REQUIRE(weapon.visible[0] && weapon.visible[4]==inserted && weapon.visible[2]==inserted);
        REQUIRE(!loose.visible[0] && loose.visible[4] && loose.visible[2]);
        REQUIRE(!weapon.visible[11] && !weapon.visible[12] && !loose.visible[11] && !loose.visible[12]);
        // Native animation copying can move the hidden copy into the well.
        // Each eye/recalculation must retain zero skinning on that copy;
        // the receiver and the canonical magazine's hierarchy remain valid.
        weapon_poses[11]=spare;
        weapon_poses[11].c.set(ak12_fixture::reload_idle_position[0],
            ak12_fixture::reload_idle_position[1],ak12_fixture::reload_idle_position[2]);
        for (int eye=0;eye<2;++eye)
        {
            weapon.CalculateBones_Invalidate(); weapon.CalculateBones(true);
            loose.CalculateBones_Invalidate(); loose.CalculateBones(true);
            vr::CollapseHiddenSkinning(weapon); vr::CollapseHiddenSkinning(loose);
            REQUIRE(Close(weapon.bones[11].mRenderTransform._11,0.f));
            REQUIRE(Close(weapon.bones[12].mRenderTransform._22,0.f));
            REQUIRE(Close(loose.bones[11].mRenderTransform._33,0.f));
            REQUIRE(Close(weapon.bones[0].mRenderTransform._11,1.f));
            REQUIRE(Close(weapon.bones[4].mRenderTransform._11,inserted ? 1.f : 0.f));
            REQUIRE(Close(weapon.bones[4].mTransform._11,1.f));
        }
    }
    // A dedicated magazine asset may omit the copy entirely.
    PistolTestModel dedicated; dedicated.names[11]="dedicated_body";
    vr::ApplyWeaponPose(dedicated,loose_poses,PistolTestCallback,2,nullptr,0.f,"bullets",
        {"magazin"},true,false,true,nullptr,{}, {},nullptr,0.f,reload_roots);
    REQUIRE(dedicated.visible[4] && dedicated.visible[2]);
    // Or its canonical/alternate bones have different IDs: cached names
    // must resolve against this skeleton without hiding receiver geometry.
    PistolTestModel reordered;
    std::swap(reordered.names[4],reordered.names[10]);
    std::swap(reordered.names[9],reordered.names[11]);
    reordered.data[2].parent=10; reordered.data[12].parent=9;
    vr::ApplyWeaponPose(reordered,loose_poses,PistolTestCallback,2,nullptr,0.f,"bullets",
        {"magazin","reload_rounds"},true,false,true,nullptr,{}, {},nullptr,0.f,reload_roots);
    REQUIRE(reordered.visible[10] && reordered.visible[2] && !reordered.visible[4]);
    REQUIRE(!reordered.visible[9] && !reordered.visible[12]);
}
static void PistolRendering()
{
    PistolTestModel pistol;std::vector<Fmatrix> poses;
    for(int frame=0;frame<500;++frame)
    {
        const bool inserted=frame%2==0;
        vr::ApplyPistolPose(pistol,poses,PistolTestCallback,2,0.035f,false,inserted,inserted);
        REQUIRE(Close(pistol.bones[7].mTransform.c.z,0.0309581f));
        REQUIRE(Close(pistol.bones[7].mRenderTransform.c.z,-0.035f));
        REQUIRE(Close(pistol.bones[7].mTransform._11,1));
        REQUIRE(pistol.visible[0]&&pistol.visible[7]&&!pistol.visible[10]);
        REQUIRE(pistol.visible[4]==inserted&&pistol.visible[8]==inserted&&pistol.visible[6]==inserted);
        pistol.CalculateBones_Invalidate();pistol.CalculateBones(true);
        REQUIRE(Close(pistol.bones[7].mRenderTransform.c.z,-0.035f));
        REQUIRE(Close(pistol.bones[0].mTransform._11,1));
    }
    vr::ApplyPistolPose(pistol,poses,PistolTestCallback,2,0,true,false,true);
    REQUIRE(!pistol.visible[0]&&!pistol.visible[7]&&pistol.visible[4]&&pistol.visible[8]);
    // The hidden body must retain a valid transform for its magazine children.
    REQUIRE(Close(pistol.bones[0].mTransform._11,1));
    REQUIRE(Close(pistol.bones[0].mTransform._22,1));
    REQUIRE(Close(pistol.bones[4].mRenderTransform._11,1)&&Close(pistol.bones[4].mRenderTransform.c.y,0));
    // The generic component must hide every seated-magazine part during
    // extraction, then render only those parts in the separate magazine OGF.
    vr::ApplyWeaponPose(pistol,poses,PistolTestCallback,2,"slide",0.f,"bullets",
        {"magazin","bullets"},false,false,false,nullptr,{},
        {"sprezyna","siodelko"});
    REQUIRE(!pistol.visible[4]&&!pistol.visible[8]&&!pistol.visible[6]&&
        !pistol.visible[1]&&pistol.visible[0]);
    // Hidden parts share the pistol mesh chunk: their skinning must collapse
    // while the hierarchy pose stays valid for children.
    REQUIRE(Close(pistol.bones[4].mRenderTransform._11,0)&&Close(pistol.bones[4].mRenderTransform._22,0));
    REQUIRE(Close(pistol.bones[4].mRenderTransform.c.y,-0.0237147f));
    REQUIRE(Close(pistol.bones[4].mTransform._11,1)&&Close(pistol.bones[0].mRenderTransform._11,1));
    // An unlisted child of the hidden magazine hides with it.
    REQUIRE(!pistol.visible[2]&&Close(pistol.bones[2].mRenderTransform._11,0));
    vr::ApplyWeaponPose(pistol,poses,PistolTestCallback,2,"slide",0.f,"bullets",
        {"magazin","bullets"},true,false,true,nullptr,{},
        {"sprezyna","siodelko"});
    REQUIRE(pistol.visible[4]&&pistol.visible[8]&&pistol.visible[6]&&
        pistol.visible[1]&&!pistol.visible[0]&&!pistol.visible[7]);
    // ...and stays with the loose magazine-only model.
    REQUIRE(pistol.visible[2]&&!pistol.visible[3]);
    // A moving part carries its children (a charging handle on a bolt), and
    // nothing else moves.
    PistolTestModel cycling;
    cycling.data[3].parent=7;
    vr::ApplyWeaponPose(cycling,poses,PistolTestCallback,2,"slide",0.02f,"bullets",
        {"magazin","bullets"},false,true,true);
    REQUIRE(Close(poses[7].c.z,0.0659581f-0.02f)&&Close(poses[3].c.z,-0.02f));
    REQUIRE(Close(poses[9].c.z,0)&&Close(poses[4].c.z,-0.0004783f));
}
static void SightOptics()
{
    // The scope lens and red dot reach the rasterizer in homogeneous clip
    // space: a point 20 cm ahead keeps w = 0.2 and a depth inside 0..1
    // (dividing by w twice put it past the far plane, so it never drew).
    Fmatrix project{};
    const float near_plane=0.03f, far_plane=1000.f;
    project._11=project._22=1.f; project._33=far_plane/(far_plane-near_plane); project._34=1.f;
    project._43=-near_plane*far_plane/(far_plane-near_plane);
    float clip[4];
    vr::ClipPosition(project,Fvector{0.01f,0,0.2f},clip);
    REQUIRE(Close(clip[3],0.2f));
    REQUIRE(clip[2]/clip[3]>0.f && clip[2]/clip[3]<1.f);
    REQUIRE(Close(clip[0]/clip[3],0.05f));
    // A red dot 100 m ahead through a 12 mm window 10 cm in front of the eye.
    const Fvector axis{0,0,1}, window{0,0,0.1f}, dot{0,0,100.f};
    REQUIRE(vr::SightWindowHit(Fvector{0,0,0},dot,window,axis,0.012f));
    // Head moved 5 mm: the dot is still seen (a collimator has no parallax
    // window to speak of at this offset)...
    REQUIRE(vr::SightWindowHit(Fvector{0.005f,0,0},dot,window,axis,0.012f));
    // ...but not 3 cm off, and never from in front of the sight.
    REQUIRE(!vr::SightWindowHit(Fvector{0.03f,0,0},dot,window,axis,0.012f));
    REQUIRE(!vr::SightWindowHit(Fvector{0,0,200.f},dot,window,axis,0.012f));
    // Scope image: full at 6 cm eye relief on axis, gone too close, too far
    // or off axis, partial in between.
    REQUIRE(Close(vr::ExitPupilFade(0.06f,0.f),1.f));
    REQUIRE(Close(vr::ExitPupilFade(0.01f,0.f),0.f));
    REQUIRE(Close(vr::ExitPupilFade(0.20f,0.f),0.f));
    REQUIRE(Close(vr::ExitPupilFade(0.06f,0.03f),0.f));
    const float partial=vr::ExitPupilFade(0.06f,0.02f);
    REQUIRE(partial>0.f && partial<1.f);
}
static void MenuAnchoring()
{
    auto& system=CVrSystem::Get();
    runtime.head_x=0;runtime.head_y=1.6f;runtime.head_yaw=0;
    REQUIRE(system.Initialize(runtime.device,1920,1080,DXGI_FORMAT_R8G8B8A8_UNORM));
    REQUIRE(system.PrepareFrame());system.SubmitFrame();
    const auto head_space=runtime.quad_space;
    runtime.queued_state=XR_SESSION_STATE_VISIBLE;system.PollEvents();
    system.SetMenuActive(true);
    runtime.head_x=0.8f;runtime.head_y=0.4f; // resting HMD must not anchor the menu
    REQUIRE(system.PrepareFrame());system.SubmitFrame();REQUIRE(runtime.layers==0);
    runtime.queued_state=XR_SESSION_STATE_FOCUSED;system.PollEvents();
    runtime.head_x=0;runtime.head_y=1.6f;
    REQUIRE(system.PrepareFrame());float u,v;REQUIRE(system.UiAim(1,u,v)&&Close(u,0.5f));
    system.SubmitFrame();const auto menu_space=runtime.quad_space;const auto anchor=runtime.quad_pose;
    REQUIRE(menu_space!=head_space&&Close(anchor.position.y,1.6f)&&Close(anchor.position.z,-1.5f));
    runtime.head_x=0.3f;runtime.head_y=1.8f;
    REQUIRE(system.PrepareFrame());system.SetMenuActive(true);
    REQUIRE(system.UiAim(1,u,v)&&Close(u,0.7f));system.SubmitFrame();
    REQUIRE(runtime.quad_space==menu_space&&Close(runtime.quad_pose.position.x,anchor.position.x));
    REQUIRE(Close(runtime.quad_pose.position.y,anchor.position.y)&&Close(runtime.quad_pose.position.z,anchor.position.z));
    runtime.head_yaw=1.57079632679f;
    REQUIRE(system.PrepareFrame());REQUIRE(!system.UiAim(1,u,v));system.SubmitFrame();
    REQUIRE(Close(runtime.quad_pose.orientation.w,1)&&Close(runtime.quad_pose.position.z,-1.5f));
    // The in-game HUD has its own distance and size (vr_hud_distance,
    // vr_hud_scale); the cursor ray and the quad agree on them.
    system.SetMenuActive(false);
    vr_hud_distance=0.7f; vr_hud_scale=0.8f;
    REQUIRE(system.PrepareFrame());REQUIRE(system.UiAim(1,u,v)&&Close(u,0.5f)&&Close(v,0.5f));system.SubmitFrame();
    REQUIRE(runtime.quad_space==head_space&&Close(runtime.quad_pose.position.z,-0.7f)&&Close(runtime.quad_width,1.2f));
    vr_hud_distance=1.f; vr_hud_scale=1.f;
    system.SetMenuActive(true);
    REQUIRE(system.PrepareFrame());system.SubmitFrame();
    REQUIRE(Close(runtime.quad_pose.position.x,-1.2f)&&Close(runtime.quad_pose.position.z,0));
    REQUIRE(Close(runtime.quad_pose.orientation.y,std::sin(runtime.head_yaw*0.5f)));
    system.Recenter();system.SetMenuActive(true); // do not capture a stale pre-recenter sample
    runtime.head_x=-0.4f;runtime.head_y=1.7f;runtime.head_yaw=0;
    REQUIRE(system.PrepareFrame());REQUIRE(system.UiAim(1,u,v)&&Close(u,0.5f));system.SubmitFrame();
    REQUIRE(Close(runtime.quad_pose.position.x,-0.4f)&&Close(runtime.quad_pose.position.y,1.7f));
    REQUIRE(Close(runtime.quad_pose.orientation.w,1)&&Close(runtime.quad_pose.position.z,-1.5f));
    system.SetMenuActive(false,true);
    REQUIRE(system.PrepareFrame());system.SubmitFrame();
    const auto inventory_anchor=runtime.quad_pose;
    REQUIRE(runtime.quad_space==menu_space&&Close(runtime.quad_width,1.92f));
    runtime.head_x=-0.1f;
    REQUIRE(system.PrepareFrame());system.SubmitFrame();
    REQUIRE(Close(runtime.quad_pose.position.x,inventory_anchor.position.x));
    system.Shutdown();runtime.head_x=0;runtime.head_y=1.6f;runtime.head_yaw=0;
}
static bool CaptureEyeWorld(const VrEyeRenderContext& eye,void* user)
{
    auto* worlds=static_cast<std::array<Fmatrix,2>*>(user);
    (*worlds)[eye.eye].invert(eye.view);return true;
}
static void HeightCalibration()
{
    auto& system=CVrSystem::Get();
    runtime.head_y=0.7f;runtime.head_x=0;
    REQUIRE(system.Initialize(runtime.device,1920,1080,DXGI_FORMAT_R8G8B8A8_UNORM));
    runtime.head_y=2.1f; // HMD raised from the startup/menu position
    REQUIRE(system.PrepareFrame());
    Fmatrix body;body.identity();body.c.y=1.65f;
    system.SetGameplayBody(body,&runtime);
    REQUIRE(Close(system.WorldHeadPosition().y,1.65f)&&system.GameplayReady());
    REQUIRE(system.RenderScene(HeightEye,nullptr));system.SubmitFrame();
    runtime.head_y=2.35f;runtime.head_x=0.2f;
    REQUIRE(system.PrepareFrame());system.SetGameplayBody(body,&runtime);
    REQUIRE(Close(system.WorldHeadPosition().y,1.9f)&&Close(system.WorldHeadPosition().x,0.2f));
    // A turn while leaning must pivot at the head, not orbit the body anchor.
    body.i.set(0,0,-1);body.k.set(1,0,0);
    system.SetGameplayBody(body,&runtime);
    REQUIRE(Close(system.WorldHeadPosition().x,0.2f)&&Close(system.WorldHeadPosition().z,0));
    // The body frame and placement compose to the world pose, and a turn
    // of the body leaves a hand's body-frame pose as it was: hand motion
    // measured there has no artificial turn in it (snap-turn spikes).
    {
        const XrPosef hand_pose{{0,0,0,1},{0.3f,1.2f,-0.4f}};
        const Fmatrix before_local=system.TrackingToBody(hand_pose);
        Fmatrix composed;composed.mul_43(system.BodyPlacement(),before_local);
        const Fmatrix world=system.TrackingToWorld(hand_pose);
        REQUIRE(Close(composed.c.x,world.c.x)&&Close(composed.c.y,world.c.y)&&Close(composed.c.z,world.c.z));
        Fmatrix turned=body;turned.i.set(0,0,1);turned.k.set(-1,0,0);
        system.SetGameplayBody(turned,&runtime);
        const Fmatrix after_local=system.TrackingToBody(hand_pose);
        const Fmatrix after_world=system.TrackingToWorld(hand_pose);
        REQUIRE(!(Close(after_world.c.x,world.c.x)&&Close(after_world.c.z,world.c.z)));
        // Only the head-pivot translation (no rotation) may change it.
        REQUIRE(Close(after_local.k.x,before_local.k.x)&&Close(after_local.k.z,before_local.k.z));
        system.SetGameplayBody(body,&runtime);
    }
    // The world head frame turns with the body; HeadPose stays in the body's.
    {
        Fmatrix head;REQUIRE(system.HeadWorld(head)&&Close(head.k.x,1)&&Close(head.k.z,0));
        Fvector p,d,u;REQUIRE(system.HeadPose(p,d,u)&&Close(d.z,1)&&Close(d.x,0));
    }
    // Roomscale: the body walks 0.3 along +x under a head that stays put.
    {
        const Fvector before=system.WorldHeadPosition();
        system.ShiftTrackingOrigin(Fvector{0.3f,0,0});
        Fmatrix moved=body;moved.c.x+=0.3f;system.SetGameplayBody(moved,&runtime);
        const Fvector after=system.WorldHeadPosition();
        REQUIRE(Close(after.x,before.x)&&Close(after.y,before.y)&&Close(after.z,before.z));
        system.ShiftTrackingOrigin(Fvector{-0.3f,0,0});system.SetGameplayBody(body,&runtime);
        REQUIRE(Close(system.WorldHeadPosition().x,before.x));
    }
    Fvector limited{0.1f,1.8f,0};system.ConstrainHead(limited);
    REQUIRE(Close(system.WorldHeadPosition().x,0.1f)&&Close(system.WorldHeadPosition().y,1.8f));
    const auto hand=system.TrackingToWorld({{0,0,0,1},{runtime.head_x,runtime.head_y,0}});
    REQUIRE(Close(hand.c.x,0.1f)&&Close(hand.c.y,1.8f));system.SubmitFrame();
    runtime.tracking=false;REQUIRE(system.PrepareFrame());system.SetGameplayBody(body,&runtime);system.SubmitFrame();
    runtime.tracking=true;runtime.head_y=5;runtime.head_x=5;
    REQUIRE(system.PrepareFrame());system.SetGameplayBody(body,&runtime);
    REQUIRE(Close(system.WorldHeadPosition().y,1.9f)&&Close(system.WorldHeadPosition().x,0.2f));
    Fmatrix menu;menu.identity();system.SetGameplayBody(menu,nullptr);REQUIRE(!system.GameplayReady());
    system.SetGameplayBody(body,&runtime);REQUIRE(Close(system.WorldHeadPosition().y,1.9f));
    system.SetGameplayBody(body,&body);REQUIRE(Close(system.WorldHeadPosition().y,1.65f));
    system.SubmitFrame();
    // A destroyed owner is forgotten: a successor at the same address is
    // calibrated again (level change, load).
    runtime.head_y+=0.4f;
    REQUIRE(system.PrepareFrame());system.SetGameplayBody(body,&body);
    REQUIRE(Close(system.WorldHeadPosition().y,2.05f));
    system.ForgetBody(&body);system.SetGameplayBody(body,&body);
    REQUIRE(Close(system.WorldHeadPosition().y,1.65f));system.SubmitFrame();
    runtime.head_x=5.4f;runtime.head_y=5.2f;
    REQUIRE(system.PrepareFrame());system.SetGameplayBody(body,&body);system.SubmitFrame();
    runtime.space_change=true;system.PollEvents();
    REQUIRE(system.PrepareFrame());system.SetGameplayBody(body,&body);
    REQUIRE(Close(system.WorldHeadPosition().x,body.c.x)&&Close(system.WorldHeadPosition().z,body.c.z));
    REQUIRE(Close(system.WorldHeadPosition().y,body.c.y));
    std::array<Fmatrix,2> centered_eyes;
    REQUIRE(system.RenderScene(CaptureEyeWorld,&centered_eyes));
    REQUIRE(Close((centered_eyes[0].c.x+centered_eyes[1].c.x)*0.5f,body.c.x));
    REQUIRE(Close((centered_eyes[0].c.z+centered_eyes[1].c.z)*0.5f,body.c.z));
    REQUIRE(Close((centered_eyes[0].c.y+centered_eyes[1].c.y)*0.5f,body.c.y));
    const float dx=centered_eyes[1].c.x-centered_eyes[0].c.x, dz=centered_eyes[1].c.z-centered_eyes[0].c.z;
    REQUIRE(Close(std::sqrt(dx*dx+dz*dz),0.062f));
    system.SubmitFrame();
    runtime.tracking=false;system.Recenter();
    REQUIRE(system.PrepareFrame());system.SetGameplayBody(body,&body);system.SubmitFrame();
    runtime.tracking=true;runtime.head_x=8;runtime.head_y=3;
    REQUIRE(system.PrepareFrame());system.SetGameplayBody(body,&body);
    REQUIRE(Close(system.WorldHeadPosition().x,body.c.x)&&Close(system.WorldHeadPosition().z,body.c.z));
    REQUIRE(Close(system.WorldHeadPosition().y,body.c.y));
    system.SubmitFrame();
    // Standing up: the eye height is taken again where the head is, without
    // moving it sideways.
    runtime.head_y=3.3f;
    REQUIRE(system.PrepareFrame());system.SetGameplayBody(body,&body);
    {
        Fvector p,d,u;REQUIRE(system.HeadPose(p,d,u)&&Close(p.y,0.3f));const float x=p.x;
        system.RecalibrateHeight();
        REQUIRE(system.HeadPose(p,d,u)&&Close(p.y,0)&&Close(p.x,x));
        REQUIRE(Close(system.WorldHeadPosition().y,body.c.y));
    }
    system.SubmitFrame();system.Shutdown();
    runtime.head_x=0;runtime.head_y=1.6f;
}
static int eye_calls=0;
static bool Eye(const VrEyeRenderContext&eye,void*)
{CheckColorView(eye.target);++eye_calls;REQUIRE(!eye.depth);REQUIRE(eye.width==144&&eye.height==160);REQUIRE(Close(eye.view._41,eye.eye==0?0.031f:-0.031f));return true;}
static bool FailedEye(const VrEyeRenderContext&,void*){return false;}
static bool DepthEye(const VrEyeRenderContext&eye,void*)
{
    REQUIRE(eye.depth);D3D11_TEXTURE2D_DESC d{};static_cast<ID3D11Texture2D*>(eye.depth)->GetDesc(&d);
    REQUIRE(int(d.Width)==eye.width&&int(d.Height)==eye.height);
    // No hand tracked within reach of the face here: the near plane is the minimum.
    REQUIRE(Close(eye.near_plane,vr_near_plane));
    return true;
}
static void BodyPolicies()
{
    // The head's heading from forward and up, also looking straight down or up.
    float yaw=1;REQUIRE(vr::HeadHeading(0,0,1,0,0,yaw)&&Close(yaw,0));
    REQUIRE(vr::HeadHeading(0,-1,0,1,0,yaw)&&Close(yaw,1.5707963f));
    REQUIRE(vr::HeadHeading(0,1,0,0,-1,yaw)&&Close(yaw,0));
    // The torso stays within the deadband and is dragged beyond it, across pi.
    REQUIRE(Close(vr::TorsoYaw(0,0.5f,0.87f),0));
    REQUIRE(Close(vr::TorsoYaw(0,1.2f,0.87f),0.33f));
    REQUIRE(Close(vr::TorsoYaw(3.f,-3.f,0.5f),3.f));
    REQUIRE(Close(vr::TorsoYaw(3.f,-2.f,0.5f),-2.5f));
    // Roomscale: leaning is not walking; the follow never overshoots the head.
    REQUIRE(vr::RoomscaleRequest(0.1f,0.15f)==0&&Close(vr::RoomscaleRequest(0.2f,0.15f),0.2f+0.05f/0.3f));
    REQUIRE(vr::RoomscaleRequest(1.f,0.15f)==1);
    REQUIRE(vr::RoomscaleFollowed(-0.1f,1)==0&&Close(vr::RoomscaleFollowed(0.3f,0.2f),0.2f)&&Close(vr::RoomscaleFollowed(0.1f,0.2f),0.1f));
    REQUIRE(vr::HeadBlockedFade(0)==0&&Close(vr::HeadBlockedFade(0.12f),1)&&vr::HeadBlockedFade(0.5f)==1&&Close(vr::HeadBlockedFade(0.07f),0.5f));
    REQUIRE(Close(vr::AdaptiveNearPlane(0.2f,0.03f,1),0.03f)&&Close(vr::AdaptiveNearPlane(1e30f,0.03f,1),0.1f));
    REQUIRE(Close(vr::AdaptiveNearPlane(0.5f,0.03f,1),0.05f)&&Close(vr::AdaptiveNearPlane(0.2f,0.15f,1),0.15f));
    // Roomscale starts at 15 cm, keeps following to 5 cm; ducking widens the start.
    REQUIRE(Close(vr::RoomscaleDeadzone(false,0),0.15f)&&Close(vr::RoomscaleDeadzone(true,0.3f),0.05f));
    REQUIRE(Close(vr::RoomscaleDeadzone(false,0.2f),0.25f)&&Close(vr::RoomscaleDeadzone(false,2.f),0.45f));
    REQUIRE(Close(vr::RoomscaleDeadzone(false,-0.3f),0.15f));
    // Gaze target: wider up close, never under 2 degrees, wider once named.
    REQUIRE(vr::GazeTargetAngle(2.f,false)>vr::GazeTargetAngle(20.f,false));
    REQUIRE(vr::GazeTargetAngle(1000.f,false)>0.035f&&vr::GazeTargetAngle(1000.f,false)<0.04f);
    REQUIRE(Close(vr::GazeTargetAngle(5.f,true),1.5f*vr::GazeTargetAngle(5.f,false)));
    REQUIRE(Close(vr::GazeTargetAngle(0.f,false),vr::GazeTargetAngle(0.5f,false)));
    REQUIRE(vr::RoomscaleRequest(0.10f,vr::RoomscaleDeadzone(false,0))==0&&vr::RoomscaleRequest(0.10f,vr::RoomscaleDeadzone(true,0))>0);
    // The torso comes back round to the head once it looks ahead again, but
    // not while looking down at the belt.
    {
        const auto rad=[](float d){ return d*3.14159265f/180.f; };
        float torso=vr::TorsoYaw(0.f,rad(90.f),rad(50.f));
        REQUIRE(Close(torso,rad(40.f)));
        torso=vr::TorsoYaw(torso,0.f,rad(50.f));
        REQUIRE(Close(torso,rad(40.f)));
        float down=torso;
        for(int i=0;i<300;++i){torso=vr::TorsoSettle(torso,0.f,1.f/90.f,false);down=vr::TorsoSettle(down,0.f,1.f/90.f,true);}
        REQUIRE(std::abs(torso)<rad(5.f)&&Close(down,rad(40.f)));
        REQUIRE(std::abs(vr::TorsoSettle(rad(170.f),rad(-170.f),1.5f,false))>rad(170.f)); // the short way round
    }
    // Menu cursor smoothing: off passes through; a still hand's tremor is
    // damped; a sweep is caught up with quickly.
    {
        vr::CursorSmoother s;
        float u=0.5f,v=0.5f;s.Step(u,v,0.011f,4.f);REQUIRE(Close(u,0.5f)&&Close(v,0.5f));
        float worst=0.f;
        for(int i=0;i<200;++i)
        {
            u=0.5f+((i&1)?0.002f:-0.002f);v=0.5f;s.Step(u,v,0.011f,4.f);
            if(i>20)worst=std::max(worst,std::abs(u-0.5f));
        }
        REQUIRE(worst<0.001f);
        for(int i=0;i<9;++i){u=0.9f;v=0.5f;s.Step(u,v,0.011f,4.f);}
        REQUIRE(std::abs(u-0.9f)<0.01f);
        u=0.1f;v=0.2f;s.Step(u,v,0.011f,0.f);REQUIRE(Close(u,0.1f)&&Close(v,0.2f));
        s.Reset();u=0.3f;v=0.3f;s.Step(u,v,0.011f,4.f);REQUIRE(Close(u,0.3f));
    }
    // Standing up: 2 s held more than 20 cm above; ducking or a jump never.
    vr::HeightRecalibration height;
    for(int i=0;i<19;++i)REQUIRE(!height.Step(true,0.3f,0.1f));
    REQUIRE(height.Step(true,0.3f,0.1f)&&!height.Step(true,0.3f,0.1f));
    height.Reset();
    for(int i=0;i<15;++i)REQUIRE(!height.Step(true,0.3f,0.1f));
    REQUIRE(!height.Step(true,0.1f,0.1f));
    for(int i=0;i<19;++i)REQUIRE(!height.Step(true,0.3f,0.1f));
    for(int i=0;i<40;++i)REQUIRE(!height.Step(true,-0.5f,0.1f)&&!height.Step(false,0.5f,0.1f));
}
static void Clean()
{
    CVrSystem::Get().Shutdown();
    REQUIRE(!runtime.instances&&!runtime.sessions&&!runtime.spaces&&!runtime.sets&&!runtime.chains);
    REQUIRE(runtime.begins==runtime.ends);
}
int main()
{
    Math();GameplayPolicies();HandRendering();PistolRendering();ReloadMagazineRegression();SightOptics();MenuPointerPolicy();PhysicsProbeAndFrameWork();Connect();
    ID3D11DeviceContext* context=nullptr;D3D_FEATURE_LEVEL level;
    REQUIRE(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&runtime.device,&level,&context)));
    IDXGIDevice*dxgi=nullptr;IDXGIAdapter*adapter=nullptr;DXGI_ADAPTER_DESC desc{};
    REQUIRE(SUCCEEDED(runtime.device->QueryInterface(__uuidof(IDXGIDevice),reinterpret_cast<void**>(&dxgi))));
    dxgi->GetAdapter(&adapter);adapter->GetDesc(&desc);runtime.luid=desc.AdapterLuid;adapter->Release();dxgi->Release();
    auto&system=CVrSystem::Get();
    runtime.loader=false;REQUIRE(!system.Prepare());Clean();runtime.loader=true;
    runtime.headset=false;REQUIRE(!system.Prepare());Clean();runtime.headset=true;
    REQUIRE(system.Initialize(runtime.device,1920,1080,DXGI_FORMAT_R8G8B8A8_UNORM));
    const int initial_syncs=runtime.syncs; // startup may pump a running frame
    REQUIRE(system.PrepareFrame());REQUIRE(!system.PrepareFrame());
    float pointer_u,pointer_v;
    REQUIRE(system.UiAim(1,pointer_u,pointer_v)&&Close(pointer_u,0.5f)&&Close(pointer_v,0.5f));
    CheckColorView(system.UiTarget());
    Fmatrix identity;identity.identity();system.SetWorldOrigin(identity);
    REQUIRE(system.RenderScene(Eye,nullptr));REQUIRE(system.RenderScene(Eye,nullptr));REQUIRE(eye_calls==2);
    REQUIRE(runtime.syncs==initial_syncs+1);system.SubmitFrame();system.SubmitFrame();REQUIRE(runtime.projection_layers==1&&runtime.layers==2);
    runtime.render=false;const int acquired=runtime.acquires;
    REQUIRE(system.PrepareFrame());system.RenderScene(Eye,nullptr);system.SubmitFrame();REQUIRE(runtime.acquires==acquired&&runtime.layers==0);
    runtime.render=true;runtime.tracking=false;REQUIRE(system.PrepareFrame());system.RenderScene(Eye,nullptr);system.SubmitFrame();REQUIRE(runtime.projection_layers==0);
    // A controller out of tracking keeps its stick (movement and turning).
    {float sx,sy;XrPosef lost;VrInput::Get().Thumbstick(0,sx,sy);REQUIRE(VrInput::Get().HandActive(0)&&!VrInput::Get().GripPose(0,lost)&&sx==0.5f&&sy==0.25f);}
    runtime.tracking=true;REQUIRE(system.PrepareFrame());system.RenderScene(Eye,nullptr);system.SubmitFrame();
    REQUIRE(VrInput::Get().Button(1)&&VrInput::Get().Grip(0)==0.75f&&VrInput::Get().HasRecentState());runtime.active=false;
    REQUIRE(system.PrepareFrame());system.SubmitFrame();float x,y;VrInput::Get().Thumbstick(0,x,y);
    REQUIRE(!VrInput::Get().Button(0)&&!VrInput::Get().Select(0)&&!VrInput::Get().HandActive(0)&&VrInput::Get().Grip(0)==0&&VrInput::Get().Trigger(0)==0&&x==0&&y==0);
    REQUIRE(!VrInput::Get().HasRecentState());
    REQUIRE(!system.UiAim(1,pointer_u,pointer_v));
    runtime.head_x=0.25f;
    REQUIRE(system.PrepareFrame());Fvector position,direction,up;
    REQUIRE(system.HeadPose(position,direction,up)&&Close(position.x,0.25f));system.SubmitFrame();
    runtime.space_change=true;runtime.head_x=5;system.PollEvents();
    REQUIRE(system.PrepareFrame());REQUIRE(system.HeadPose(position,direction,up)&&Close(position.x,0));system.SubmitFrame();
    system.Recenter();REQUIRE(system.PrepareFrame());REQUIRE(system.HeadPose(position,direction,up)&&Close(position.x,0));system.SubmitFrame();
    REQUIRE(system.PrepareFrame());system.RenderScene(FailedEye,nullptr);system.SubmitFrame();REQUIRE(runtime.projection_layers==0);
    runtime.active=true;runtime.wait_image=XR_TIMEOUT_EXPIRED;const int released=runtime.releases;
    REQUIRE(system.PrepareFrame());REQUIRE(system.NeedsRecovery());system.SubmitFrame();REQUIRE(runtime.releases==released);Clean();
    runtime.wait_image=XR_SUCCESS;runtime.fail_chain=2;
    REQUIRE(!system.Initialize(runtime.device,1920,1080,DXGI_FORMAT_R8G8B8A8_UNORM));Clean();runtime.fail_chain=0;
    runtime.fail_action=3;REQUIRE(!system.Initialize(runtime.device,1920,1080,DXGI_FORMAT_R8G8B8A8_UNORM));Clean();runtime.fail_action=0;
    REQUIRE(system.Initialize(runtime.device,1920,1080,DXGI_FORMAT_R8G8B8A8_UNORM));
    REQUIRE(system.PrepareFrame()&&VrInput::Get().HasRecentState());system.SubmitFrame();
    runtime.wait_frame=XR_TIMEOUT_EXPIRED;const int before_timeout=runtime.begins;
    REQUIRE(!system.PrepareFrame()&&runtime.begins==before_timeout);
    REQUIRE(!VrInput::Get().HasRecentState()&&!VrInput::Get().Button(1));
    runtime.wait_frame=XR_SUCCESS;
    runtime.wait_frame=XR_ERROR_SESSION_LOST;const int begun=runtime.begins;
    REQUIRE(!system.PrepareFrame());REQUIRE(system.NeedsRecovery()&&runtime.begins==begun);Clean();
    runtime.wait_frame=XR_SUCCESS;runtime.head_x=0;
    REQUIRE(system.Initialize(runtime.device,1920,1080,DXGI_FORMAT_R8G8B8A8_UNORM));
    // STOPPING ends the session and waits for READY; it is not a lost session.
    REQUIRE(system.PrepareFrame());runtime.stopping=true;system.PollEvents();
    REQUIRE(!system.NeedsRecovery()&&!system.IsActive()&&!runtime.begun&&!runtime.running);
    REQUIRE(!system.PrepareFrame());
    runtime.queued_state=XR_SESSION_STATE_READY;system.PollEvents();
    REQUIRE(system.IsActive()&&runtime.running);
    runtime.queued_state=XR_SESSION_STATE_FOCUSED;system.PollEvents();
    REQUIRE(system.PrepareFrame()&&VrInput::Get().HasRecentState());system.SubmitFrame();Clean();
    REQUIRE(system.Initialize(runtime.device,1920,1080,DXGI_FORMAT_R8G8B8A8_UNORM));
    runtime.begin_frame=XR_SESSION_LOSS_PENDING;
    REQUIRE(!system.PrepareFrame());REQUIRE(system.NeedsRecovery()&&!runtime.begun);Clean();runtime.begin_frame=XR_SUCCESS;
    runtime.color_format=DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    REQUIRE(system.Initialize(runtime.device,1920,1080,DXGI_FORMAT_R8G8B8A8_UNORM));
    REQUIRE(system.PrepareFrame());system.RenderScene(Eye,nullptr);system.SubmitFrame();Clean();
    runtime.color_format=DXGI_FORMAT_R8G8B8A8_UNORM;
    REQUIRE(!system.Initialize(runtime.device,1920,1080,DXGI_FORMAT_R8G8B8A8_UNORM));Clean();
    runtime.color_format=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    HeightCalibration();Clean();MenuAnchoring();Clean();
    BodyPolicies();
    // Depth layer: offered depth swapchains are filled per eye and submitted.
    runtime.depth=true;
    REQUIRE(system.Initialize(runtime.device,1920,1080,DXGI_FORMAT_R8G8B8A8_UNORM));
    REQUIRE(runtime.chains==5&&system.PrepareFrame());system.SetWorldOrigin(identity);
    REQUIRE(system.RenderScene(DepthEye,nullptr));system.SubmitFrame();
    REQUIRE(runtime.projection_layers==1&&runtime.depth_views==2);
    REQUIRE(system.PrepareFrame());system.RenderScene(FailedEye,nullptr);system.SubmitFrame();
    REQUIRE(runtime.depth_views==0);
    Clean();REQUIRE(runtime.chains==0);runtime.depth=false;
    runtime.no_ready=true;const auto timeout_start=GetTickCount64();
    REQUIRE(!system.Initialize(runtime.device,1920,1080,DXGI_FORMAT_R8G8B8A8_UNORM));
    REQUIRE(GetTickCount64()-timeout_start>=5000&&GetTickCount64()-timeout_start<10000);Clean();
    context->Release();runtime.device->Release();
    puts("PASS: arguments, coordinates, asymmetric projection, stereo dispatch, sRGB/UNORM color contract, height calibration, turn pivot, collision sweep, sticks, lifecycle, cleanup, inactive input, tracking loss, timeout, session loss");
}
