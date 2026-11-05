#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>

struct ID3D11Device;
struct ID3D11DeviceContext;

#ifdef ENGINE_API
#define VR_PERF_API ENGINE_API
#else
#define VR_PERF_API
#endif

namespace vr
{
enum class PerfStage : unsigned
{
    WaitFrame, BeginFrame, Tracking, Input, UiWait, Simulation, FrameWork,
    Visibility, Geometry, QueryWait, Sun, LocalLights, Post, LeftEye, RightEye,
    // EyeMirror is nested in LeftEye; UiMirror is top level (FinishStereoFrame).
    Scope, EyeWait, DepthCopy, EyeMirror, UiMirror, EndFrame, Present, Tail,
    // Consecutive parts of Post (CRenderTarget::phase_combine), in order:
    // AO, sky and clouds, deferred combine, water/SSR/rain, forward
    // (transparents, particles, volumetrics), bloom and distortion,
    // sunshafts/fog/motion blur, blur/bloom/DOF, LUT and vision effects,
    // SMAA/TAA, final combine and post-process.
    PostAo, PostSky, PostCombine, PostWater, PostForward, PostBloom, PostScatter,
    PostBlur, PostGrade, PostAa, PostFinal,
    // Parts of LocalLights (CRender::render_lights): shadow-map setup and
    // rendering, and light accumulation. Volumetric (per shadowed spot) and
    // indirect light (GI) are nested in LightAccum.
    LightShadow, LightAccum, LightVolumetric, LightIndirect, Count
};
constexpr unsigned PerfStageCount = static_cast<unsigned>(PerfStage::Count);
constexpr std::size_t PerfMaxFrames = 4096;
enum class PerfGpuState : unsigned
{
    Unavailable, Pending, Ready, Busy, Disjoint, Error, Overflow, DeviceLost
};
struct PerfSample
{
    std::uint64_t sequence = 0;
    unsigned engine_frame = 0, width = 0, height = 0;
    std::int64_t predicted_time = 0;
    double start_ms = 0, interval_ms = -1, elapsed_ms = 0, period_ms = 0;
    double cpu_ms[PerfStageCount]{};
    double gpu_ms[PerfStageCount]{};
    double gpu_total_ms = -1;
    std::uint64_t cpu_mask = 0, gpu_mask = 0;
    unsigned draws[3]{}, shadow_lights = 0;
    int wait_result = (-2147483647 - 1), begin_result = (-2147483647 - 1), end_result = (-2147483647 - 1);
    PerfGpuState gpu_state = PerfGpuState::Unavailable;
    bool should_render = false, desktop_active = false, scene_rendered = false, aborted = false;
};

// All control/device/frame operations belong to the engine's main thread.
// Worker scopes/counters are ignored before any shared state is examined.
VR_PERF_API void PerfAttach(ID3D11Device*, ID3D11DeviceContext*);
VR_PERF_API void PerfDetach();
VR_PERF_API void PerfRequestStart();
VR_PERF_API void PerfRequestStop();
VR_PERF_API void PerfPrintStatus();
VR_PERF_API void PerfBeginFrame(unsigned engine_frame);
VR_PERF_API void PerfSetEngineFrame(unsigned);
VR_PERF_API void PerfFrameInfo(unsigned width, unsigned height, std::int64_t predicted_time,
    double period_ms, bool should_render, bool desktop_active);
VR_PERF_API void PerfXrResult(PerfStage, int);
VR_PERF_API void PerfSceneRendered(bool);
VR_PERF_API void PerfBeginGpuFrame();
VR_PERF_API void PerfEndGpuFrame();
VR_PERF_API void PerfEndFrame();
VR_PERF_API void PerfAddDraws(unsigned);
VR_PERF_API void PerfAddShadowLights(unsigned);
VR_PERF_API bool PerfFrameActive();
#ifdef XR_VR_TEST
VR_PERF_API const PerfSample* PerfSamples(std::size_t& count);
VR_PERF_API void PerfResetForTests();
#endif

class VR_PERF_API PerfScope
{
public:
    explicit PerfScope(PerfStage, bool gpu = false);
    ~PerfScope();
    PerfScope(const PerfScope&) = delete;
    PerfScope& operator=(const PerfScope&) = delete;
private:
    std::int64_t m_start = 0;
    std::uint64_t m_generation = 0, m_device_generation = 0;
    std::size_t m_sample = PerfMaxFrames;
    unsigned m_stage = 0, m_view = 3;
    int m_packet = -1, m_event = -1;
};

// Consecutive GPU-timed parts of one function body: Next ends the current
// part and starts the next; the last ends with the section.
class PerfSection
{
public:
    void Next(PerfStage stage) { m_scope.reset(); m_scope.emplace(stage, true); }
private:
    std::optional<PerfScope> m_scope;
};

class VR_PERF_API PerfViewScope
{
public:
    explicit PerfViewScope(unsigned view);
    ~PerfViewScope();
    PerfViewScope(const PerfViewScope&) = delete;
    PerfViewScope& operator=(const PerfViewScope&) = delete;
private:
    unsigned m_previous = 3;
    std::uint64_t m_generation = 0;
    std::size_t m_sample = PerfMaxFrames;
};
}
#undef VR_PERF_API
