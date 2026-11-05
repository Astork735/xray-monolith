#ifdef XR_VR_TEST
#include "../../../tests/vr/support.h"
#else
#include "../stdafx.h"
#endif
#include "vr_performance.h"

#ifdef VR_ENABLED
#include <d3d11.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>

namespace vr
{
namespace
{
constexpr unsigned PacketCount = 8, EventCount = 256, NoView = 3;
constexpr unsigned TotalStage = PerfStageCount;
static_assert(PerfStageCount <= 64, "Performance masks must cover every stage");
const char* const names[] = {"wait_frame", "begin_frame", "tracking", "input", "ui_wait",
    "simulation", "frame_work", "visibility", "geometry", "query_wait", "sun", "local_lights",
    "post", "left_eye", "right_eye", "scope", "eye_wait", "depth_copy", "eye_mirror", "ui_mirror",
    "end_frame", "present", "tail", "post_ao", "post_sky", "post_combine", "post_water",
    "post_forward", "post_bloom", "post_scatter", "post_blur", "post_grade", "post_aa", "post_final",
    "light_shadow", "light_accum", "light_volumetric", "light_indirect"};
static_assert(sizeof(names) / sizeof(*names) == PerfStageCount, "Update performance column names");
struct Event
{
    ID3D11Query* begin = nullptr;
    ID3D11Query* end = nullptr;
    unsigned stage = TotalStage;
    bool ended = false;
};
struct Packet
{
    ID3D11Query* disjoint = nullptr;
    Event events[EventCount];
    unsigned count = 0;
    std::size_t sample = PerfMaxFrames;
    std::uint64_t generation = 0;
    bool begun = false, pending = false;
};
enum class Request { None, Start, Stop };
struct Capture
{
    PerfSample samples[PerfMaxFrames];
    Packet packets[PacketCount];
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    std::size_t count = 0;
    std::uint64_t generation = 0, device_generation = 0;
    std::int64_t frequency = 0, origin = 0, previous = 0, frame_start = 0;
    unsigned cursor = 0;
    int active_packet = -1;
    bool recording = false, gpu_available = false, dumped = false;
    Request request = Request::None;
} capture;
thread_local PerfSample* frame = nullptr;
thread_local unsigned view = NoView;

std::int64_t Ticks()
{
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}
double Milliseconds(std::int64_t ticks)
{
    return capture.frequency > 0 ? double(ticks) * 1000.0 / double(capture.frequency) : 0.0;
}
template<class T> void Release(T*& object)
{
    if (object) object->Release();
    object = nullptr;
}
PerfSample* PacketSample(const Packet& packet)
{
    return packet.generation == capture.generation && packet.sample < capture.count ?
        &capture.samples[packet.sample] : nullptr;
}
void ClosePacket(Packet& packet)
{
    if (!packet.begun || !capture.context) return;
    for (unsigned i = 0; i < packet.count; ++i)
        if (!packet.events[i].ended)
        {
            capture.context->End(packet.events[i].end);
            packet.events[i].ended = true;
        }
    capture.context->End(packet.disjoint);
    packet.begun = false;
    packet.pending = true;
}
void ReleaseQueries(bool lost)
{
    for (auto& packet : capture.packets)
    {
        ClosePacket(packet);
        if ((packet.pending || packet.begun) && lost)
            if (auto* sample = PacketSample(packet))
            {
                sample->gpu_state = PerfGpuState::DeviceLost;
                sample->gpu_mask = 0;
                sample->gpu_total_ms = -1;
            }
        Release(packet.disjoint);
        for (auto& event : packet.events) { Release(event.begin); Release(event.end); }
        packet = {};
    }
    capture.active_packet = -1;
    capture.gpu_available = false;
}
bool CreateQueries()
{
    if (!capture.device || !capture.context) return false;
    D3D11_QUERY_DESC desc{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    for (auto& packet : capture.packets)
    {
        if (FAILED(capture.device->CreateQuery(&desc, &packet.disjoint))) return false;
        desc.Query = D3D11_QUERY_TIMESTAMP;
        for (auto& event : packet.events)
            if (FAILED(capture.device->CreateQuery(&desc, &event.begin)) ||
                FAILED(capture.device->CreateQuery(&desc, &event.end))) return false;
        desc.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
    }
    return true;
}
void PollPacket(Packet& packet)
{
    if (!packet.pending || !capture.context) return;
    auto* sample = PacketSample(packet);
    if (!sample) { packet.pending = false; return; }
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
    HRESULT result = capture.context->GetData(packet.disjoint, &disjoint, sizeof(disjoint),
        D3D11_ASYNC_GETDATA_DONOTFLUSH);
    if (result == S_FALSE) return;
    if (FAILED(result) || disjoint.Disjoint || !disjoint.Frequency)
    {
        sample->gpu_state = FAILED(result) ? PerfGpuState::Error : PerfGpuState::Disjoint;
        packet.pending = false;
        return;
    }
    double stages[PerfStageCount]{};
    double total = -1;
    std::uint64_t mask = 0;
    for (unsigned i = 0; i < packet.count; ++i)
    {
        UINT64 begin = 0, end = 0;
        result = capture.context->GetData(packet.events[i].begin, &begin, sizeof(begin),
            D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (result == S_FALSE) return;
        if (SUCCEEDED(result)) result = capture.context->GetData(packet.events[i].end, &end, sizeof(end),
            D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (result == S_FALSE) return;
        if (FAILED(result) || end < begin)
        {
            sample->gpu_state = PerfGpuState::Error;
            packet.pending = false;
            return;
        }
        const double elapsed = double(end - begin) * 1000.0 / double(disjoint.Frequency);
        const unsigned stage = packet.events[i].stage;
        if (stage == TotalStage) total = elapsed;
        else { stages[stage] += elapsed; mask |= std::uint64_t(1) << stage; }
    }
    // An overflowed packet still holds valid events (every one recorded was
    // ended): keep them, and the Overflow state marks the frame as partial.
    std::copy(stages, stages + PerfStageCount, sample->gpu_ms);
    sample->gpu_total_ms = total;
    sample->gpu_mask = mask;
    if (sample->gpu_state != PerfGpuState::Overflow) sample->gpu_state = PerfGpuState::Ready;
    packet.pending = false;
}
void PollQueries()
{
    for (auto& packet : capture.packets) PollPacket(packet);
}
void Append(char* line, std::size_t size, std::size_t& used, const char* format, ...)
{
    if (used >= size) return;
    va_list arguments;
    va_start(arguments, format);
    const int count = std::vsnprintf(line + used, size - used, format, arguments);
    va_end(arguments);
    if (count < 0 || std::size_t(count) >= size - used) used = size;
    else used += std::size_t(count);
}
void Dump()
{
    if (capture.dumped) return;
    // Explicit stop only: never perform log I/O during a captured interval.
    PollQueries();
    Msg("* [VR perf] capture,%llu,frames,%u,version,4", static_cast<unsigned long long>(capture.generation),
        static_cast<unsigned>(capture.count));
    char line[2000];
    std::size_t used = 0;
    Append(line, sizeof(line), used, "columns,sequence,engine_frame,start_ms,interval_ms,elapsed_ms,period_ms,"
        "predicted_time,width,height,should_render,scene_rendered,desktop_active,aborted,wait_result,begin_result,"
        "end_result,gpu_state,gpu_total_ms,cpu_mask,gpu_mask,draws_left,draws_right,draws_scope,shadow_lights");
    for (unsigned stage = 0; stage < PerfStageCount; ++stage) Append(line, sizeof(line), used, ",cpu_%s_ms", names[stage]);
    for (unsigned stage = 0; stage < PerfStageCount; ++stage) Append(line, sizeof(line), used, ",gpu_%s_ms", names[stage]);
    if (used < sizeof(line)) Msg("* [VR perf] %s", line);
    for (std::size_t i = 0; i < capture.count; ++i)
    {
        const auto& s = capture.samples[i];
        used = 0;
        Append(line, sizeof(line), used,
            "frame,%llu,%u,%.6f,%.6f,%.6f,%.6f,%lld,%u,%u,%u,%u,%u,%u,%d,%d,%d,%u,%.6f,%llu,%llu,%u,%u,%u,%u",
            static_cast<unsigned long long>(s.sequence), s.engine_frame, s.start_ms, s.interval_ms, s.elapsed_ms,
            s.period_ms, static_cast<long long>(s.predicted_time), s.width, s.height, unsigned(s.should_render),
            unsigned(s.scene_rendered), unsigned(s.desktop_active), unsigned(s.aborted), s.wait_result,
            s.begin_result, s.end_result, unsigned(s.gpu_state), s.gpu_total_ms,
            static_cast<unsigned long long>(s.cpu_mask), static_cast<unsigned long long>(s.gpu_mask), s.draws[0], s.draws[1], s.draws[2], s.shadow_lights);
        for (unsigned stage = 0; stage < PerfStageCount; ++stage)
            Append(line, sizeof(line), used, ",%.6f", s.cpu_ms[stage]);
        for (unsigned stage = 0; stage < PerfStageCount; ++stage)
            Append(line, sizeof(line), used, ",%.6f", s.gpu_mask & (std::uint64_t(1) << stage) ? s.gpu_ms[stage] : -1.0);
        if (used < sizeof(line)) Msg("* [VR perf] %s", line);
    }
    Msg("* [VR perf] end,%llu", static_cast<unsigned long long>(capture.generation));
    capture.dumped = true;
}
void FinishFrame(bool aborted)
{
    if (!frame) return;
    PerfEndGpuFrame();
    frame->elapsed_ms = Milliseconds(Ticks() - capture.frame_start);
    frame->aborted = frame->aborted || aborted;
    frame = nullptr;
    view = NoView;
    if (capture.count == PerfMaxFrames) capture.recording = false;
}
}

void PerfAttach(ID3D11Device* device, ID3D11DeviceContext* context)
{
    PerfDetach();
    capture.device = device;
    capture.context = context;
}
void PerfDetach()
{
    ++capture.device_generation;
    ReleaseQueries(true);
    capture.device = nullptr;
    capture.context = nullptr;
    if (frame) frame->aborted = true;
    capture.recording = false;
}
void PerfRequestStart()
{
    if (capture.recording || capture.request == Request::Start) return;
    capture.request = Request::Start;
}
void PerfRequestStop() { capture.request = Request::Stop; }
void PerfPrintStatus()
{
    const char* state = capture.request == Request::Start ? "start requested" :
        capture.recording ? "recording" :
        capture.count && !capture.dumped ? "stopped, not dumped (stop writes it to the log)" : "idle/ready";
    Msg("* [VR perf] %s: %u/%u frames; vr_perf_capture start|stop|status", state,
        static_cast<unsigned>(capture.count), static_cast<unsigned>(PerfMaxFrames));
}
void PerfBeginFrame(unsigned engine_frame)
{
    FinishFrame(true);
    if (capture.request == Request::Start)
    {
        if (capture.count && !capture.dumped)
            Msg("! [VR perf] discarding capture %llu (%u frames) that was never dumped",
                static_cast<unsigned long long>(capture.generation), static_cast<unsigned>(capture.count));
        ReleaseQueries(false);
        ++capture.generation;
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        capture.frequency = frequency.QuadPart;
        capture.count = 0;
        capture.cursor = 0;
        capture.previous = 0;
        capture.dumped = false;
        capture.gpu_available = CreateQueries();
        if (!capture.gpu_available) ReleaseQueries(false);
        capture.origin = Ticks();
        capture.recording = capture.frequency > 0;
        capture.request = Request::None;
    }
    if (!capture.recording || capture.request == Request::Stop) return;
    PollQueries();
    if (capture.count >= PerfMaxFrames) { capture.recording = false; return; }
    const auto now = Ticks();
    frame = &capture.samples[capture.count++];
    *frame = {};
    frame->sequence = capture.count;
    frame->engine_frame = engine_frame;
    frame->start_ms = Milliseconds(now - capture.origin);
    frame->interval_ms = capture.previous ? Milliseconds(now - capture.previous) : -1;
    capture.previous = capture.frame_start = now;
    view = NoView;
}
void PerfSetEngineFrame(unsigned value) { if (frame) frame->engine_frame = value; }
void PerfFrameInfo(unsigned width, unsigned height, std::int64_t predicted_time, double period_ms,
    bool should_render, bool desktop_active)
{
    if (!frame) return;
    frame->width = width; frame->height = height; frame->predicted_time = predicted_time;
    frame->period_ms = period_ms; frame->should_render = should_render; frame->desktop_active = desktop_active;
}
void PerfXrResult(PerfStage stage, int result)
{
    if (!frame) return;
    if (stage == PerfStage::WaitFrame) frame->wait_result = result;
    else if (stage == PerfStage::BeginFrame) frame->begin_result = result;
    else if (stage == PerfStage::EndFrame) frame->end_result = result;
}
void PerfSceneRendered(bool value) { if (frame) frame->scene_rendered = value; }
void PerfBeginGpuFrame()
{
    if (!frame || !capture.gpu_available || capture.active_packet >= 0) return;
    const unsigned index = capture.cursor++ % PacketCount;
    auto& packet = capture.packets[index];
    if (packet.pending) { frame->gpu_state = PerfGpuState::Busy; return; }
    packet.sample = std::size_t(frame - capture.samples);
    packet.generation = capture.generation;
    packet.count = 1;
    packet.begun = true;
    packet.events[0].stage = TotalStage;
    packet.events[0].ended = false;
    capture.context->Begin(packet.disjoint);
    capture.context->End(packet.events[0].begin);
    capture.active_packet = int(index);
    frame->gpu_state = PerfGpuState::Pending;
}
void PerfEndGpuFrame()
{
    if (!frame || capture.active_packet < 0) return;
    ClosePacket(capture.packets[capture.active_packet]);
    capture.active_packet = -1;
}
void PerfEndFrame()
{
    FinishFrame(false);
    if (capture.request == Request::Stop)
    {
        capture.recording = false;
        capture.request = Request::None;
        Dump();
    }
}
void PerfAddDraws(unsigned value) { if (frame && view < 3) frame->draws[view] += value; }
void PerfAddShadowLights(unsigned value) { if (frame) frame->shadow_lights += value; }
bool PerfFrameActive() { return frame != nullptr; }

PerfScope::PerfScope(PerfStage stage, bool gpu) : m_stage(static_cast<unsigned>(stage))
{
    // TLS gate precedes every access to mutable main-thread capture state.
    if (!frame || m_stage >= PerfStageCount) return;
    m_sample = std::size_t(frame - capture.samples);
    m_generation = capture.generation;
    m_device_generation = capture.device_generation;
    m_view = view;
    m_start = Ticks();
    if (gpu && capture.active_packet >= 0)
    {
        m_packet = capture.active_packet;
        auto& packet = capture.packets[capture.active_packet];
        if (packet.count == EventCount) { frame->gpu_state = PerfGpuState::Overflow; return; }
        m_event = int(packet.count++);
        auto& event = packet.events[m_event];
        event.stage = m_stage;
        event.ended = false;
        capture.context->End(event.begin);
    }
}
PerfScope::~PerfScope()
{
    if (!frame || m_sample >= PerfMaxFrames || frame != &capture.samples[m_sample] ||
        m_generation != capture.generation) return;
    frame->cpu_ms[m_stage] += Milliseconds(Ticks() - m_start);
    frame->cpu_mask |= std::uint64_t(1) << m_stage;
    if (m_event >= 0 && m_device_generation == capture.device_generation &&
        capture.active_packet == m_packet)
    {
        auto& event = capture.packets[capture.active_packet].events[m_event];
        if (!event.ended) { capture.context->End(event.end); event.ended = true; }
    }
}
PerfViewScope::PerfViewScope(unsigned value)
{
    if (!frame) return;
    m_previous = view;
    m_generation = capture.generation;
    m_sample = std::size_t(frame - capture.samples);
    view = value < 3 ? value : NoView;
}
PerfViewScope::~PerfViewScope()
{
    if (frame && m_sample < PerfMaxFrames && frame == &capture.samples[m_sample] && m_generation == capture.generation)
        view = m_previous;
}
#ifdef XR_VR_TEST
const PerfSample* PerfSamples(std::size_t& count) { count = capture.count; return capture.samples; }
void PerfResetForTests()
{
    FinishFrame(true);
    PerfDetach();
    ++capture.generation;
    capture.count = 0;
    capture.request = Request::None;
    capture.dumped = false;
    capture.previous = 0;
}
#endif
}
#else
namespace vr
{
void PerfAttach(ID3D11Device*, ID3D11DeviceContext*) {}
void PerfDetach() {}
void PerfRequestStart() {}
void PerfRequestStop() {}
void PerfPrintStatus() {}
void PerfBeginFrame(unsigned) {}
void PerfSetEngineFrame(unsigned) {}
void PerfFrameInfo(unsigned, unsigned, std::int64_t, double, bool, bool) {}
void PerfXrResult(PerfStage, int) {}
void PerfSceneRendered(bool) {}
void PerfBeginGpuFrame() {}
void PerfEndGpuFrame() {}
void PerfEndFrame() {}
void PerfAddDraws(unsigned) {}
void PerfAddShadowLights(unsigned) {}
bool PerfFrameActive() { return false; }
PerfScope::PerfScope(PerfStage, bool) {}
PerfScope::~PerfScope() {}
PerfViewScope::PerfViewScope(unsigned) {}
PerfViewScope::~PerfViewScope() {}
#ifdef XR_VR_TEST
const PerfSample* PerfSamples(std::size_t& count) { count = 0; return nullptr; }
void PerfResetForTests() {}
#endif
}
#endif
