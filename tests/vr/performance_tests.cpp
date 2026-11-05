#include "support.h"
#include "../../src/xrEngine/Vr/vr_performance.h"
#include <cstdarg>
#include <fstream>
#include <string>
#include <thread>

static std::vector<std::string> lines;
void Msg(const char* format, ...)
{
    char line[2048];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    lines.emplace_back(line);
}
#define REQUIRE(value) do { if (!(value)) { std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #value); std::exit(1); } } while (0)
static std::uint64_t Bit(vr::PerfStage stage) { return std::uint64_t(1) << unsigned(stage); }
static const vr::PerfSample* Samples(std::size_t expected)
{
    std::size_t count = 0;
    const auto* rows = vr::PerfSamples(count);
    REQUIRE(count == expected);
    return rows;
}
static void Scene()
{
    vr::PerfFrameInfo(2112, 2304, 987654321012345678LL, 11.111, true, true);
    vr::PerfXrResult(vr::PerfStage::WaitFrame, 0);
    vr::PerfXrResult(vr::PerfStage::BeginFrame, 0);
    vr::PerfXrResult(vr::PerfStage::EndFrame, 0);
    vr::PerfSceneRendered(true);
}
static void Reset()
{
    vr::PerfResetForTests();
    lines.clear();
}
static void DisabledAndCpuOnly()
{
    Reset();
    vr::PerfBeginFrame(1);
    { vr::PerfScope timing(vr::PerfStage::Simulation, true); }
    vr::PerfBeginGpuFrame();
    vr::PerfEndGpuFrame();
    vr::PerfEndFrame();
    Samples(0);
    REQUIRE(lines.empty());

    vr::PerfRequestStart();
    vr::PerfBeginFrame(6);
    REQUIRE(vr::PerfFrameActive());
    Scene();
    vr::PerfSetEngineFrame(7);
    {
        vr::PerfScope simulation(vr::PerfStage::Simulation);
        { vr::PerfViewScope view(0); vr::PerfAddDraws(11); }
        {
            vr::PerfViewScope view(1); vr::PerfAddDraws(22);
            { vr::PerfViewScope scope(2); vr::PerfAddDraws(33); }
            vr::PerfAddDraws(1);
        }
        vr::PerfAddDraws(1000); // no current view: must not become an eye draw
        vr::PerfAddShadowLights(12);
    }
    vr::PerfEndFrame();
    const auto& row = Samples(1)[0];
    REQUIRE(row.engine_frame == 7 && row.sequence == 1);
    REQUIRE(row.cpu_mask == Bit(vr::PerfStage::Simulation));
    REQUIRE(row.gpu_mask == 0 && row.gpu_total_ms < 0);
    REQUIRE(row.gpu_state == vr::PerfGpuState::Unavailable);
    REQUIRE(row.draws[0] == 11 && row.draws[1] == 23 && row.draws[2] == 33);
    REQUIRE(row.shadow_lights == 12 && row.width == 2112 && row.predicted_time == 987654321012345678LL);
    REQUIRE(row.elapsed_ms >= row.cpu_ms[unsigned(vr::PerfStage::Simulation)]);
    REQUIRE(row.interval_ms < 0 && lines.empty());
    vr::PerfRequestStop(); vr::PerfEndFrame();
    REQUIRE(!lines.empty());
    for (const auto& line : lines) REQUIRE(line.size() < 2048);
}
static void BoundaryAndWorkerSafety()
{
    Reset(); vr::PerfRequestStart(); vr::PerfBeginFrame(10); Scene();
    std::thread worker([]
    {
        vr::PerfScope timing(vr::PerfStage::Tracking, true);
        vr::PerfViewScope view(0);
        vr::PerfAddDraws(999);
        vr::PerfAddShadowLights(999);
    });
    worker.join();
    {
        vr::PerfScope timing(vr::PerfStage::Simulation);
        vr::PerfRequestStop(); // command can arrive while scopes still exist
        REQUIRE(lines.empty() && vr::PerfFrameActive());
    }
    vr::PerfEndFrame();
    const auto& row = Samples(1)[0];
    REQUIRE(row.cpu_mask == Bit(vr::PerfStage::Simulation));
    REQUIRE(row.draws[0] == 0 && row.shadow_lights == 0 && !lines.empty());
    vr::PerfBeginFrame(11); vr::PerfEndFrame(); Samples(1);

    Reset(); vr::PerfRequestStart(); vr::PerfBeginFrame(20);
    {
        vr::PerfScope stale(vr::PerfStage::Scope, true);
        vr::PerfResetForTests();
        vr::PerfRequestStart(); vr::PerfBeginFrame(21); Scene();
    }
    vr::PerfEndFrame();
    REQUIRE(Samples(1)[0].cpu_mask == 0); // no stale destructor writes into recycled row
    vr::PerfRequestStop(); vr::PerfEndFrame();
}
static void CapacityAndFailures()
{
    Reset(); vr::PerfRequestStart();
    for (unsigned i = 0; i < vr::PerfMaxFrames + 8; ++i)
    {
        vr::PerfBeginFrame(i); Scene(); vr::PerfEndFrame();
    }
    const auto* rows = Samples(vr::PerfMaxFrames);
    REQUIRE(rows[0].engine_frame == 0 && rows[vr::PerfMaxFrames - 1].engine_frame == vr::PerfMaxFrames - 1);
    REQUIRE(lines.empty()); // full capture freezes without a surprise log-I/O hitch
    vr::PerfRequestStop(); vr::PerfEndFrame();
    REQUIRE(!lines.empty());

    Reset(); vr::PerfRequestStart(); vr::PerfBeginFrame(30);
    vr::PerfXrResult(vr::PerfStage::WaitFrame, -1);
    vr::PerfEndFrame();
    REQUIRE(Samples(1)[0].wait_result == -1 && Samples(1)[0].begin_result < 0);
    vr::PerfBeginFrame(31);
    vr::PerfFrameInfo(1, 1, 1, 10., false, false);
    vr::PerfXrResult(vr::PerfStage::WaitFrame, 0);
    vr::PerfXrResult(vr::PerfStage::BeginFrame, 0);
    vr::PerfXrResult(vr::PerfStage::EndFrame, 0);
    vr::PerfEndFrame();
    REQUIRE(!Samples(2)[1].should_render && !Samples(2)[1].scene_rendered);
    vr::PerfRequestStop(); vr::PerfEndFrame();
}
static void CreateWarp(ID3D11Device*& device, ID3D11DeviceContext*& context)
{
    D3D_FEATURE_LEVEL feature{};
    REQUIRE(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &device, &feature, &context)));
}
// A real xrEndFrame submits work. The harness has no runtime/present, so
// explicitly submit and wait here, outside the collector.
static void FinishGpu(ID3D11Device* device, ID3D11DeviceContext* context)
{
    ID3D11Query* fence = nullptr;
    D3D11_QUERY_DESC fence_desc{D3D11_QUERY_EVENT, 0};
    REQUIRE(SUCCEEDED(device->CreateQuery(&fence_desc, &fence)));
    context->End(fence);
    context->Flush();
    const auto deadline = GetTickCount64() + 5000;
    HRESULT ready;
    while ((ready = context->GetData(fence, nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH)) == S_FALSE &&
        GetTickCount64() < deadline) Sleep(1);
    REQUIRE(ready == S_OK);
    fence->Release();
}
static void GpuOverflowKeepsPartial()
{
    Reset();
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    CreateWarp(device, context);
    vr::PerfAttach(device, context); vr::PerfRequestStart();
    vr::PerfBeginFrame(1); Scene(); vr::PerfBeginGpuFrame();
    // More GPU scopes than a packet holds: the ones recorded before the
    // overflow stay valid and are kept.
    for (unsigned i = 0; i < 300; ++i) { vr::PerfScope geometry(vr::PerfStage::Geometry, true); }
    vr::PerfEndGpuFrame(); vr::PerfEndFrame();
    FinishGpu(device, context);
    vr::PerfBeginFrame(2); // polls the completed packet
    const auto& row = Samples(2)[0];
    REQUIRE(row.gpu_state == vr::PerfGpuState::Overflow);
    REQUIRE(row.gpu_total_ms >= 0 && (row.gpu_mask & Bit(vr::PerfStage::Geometry)));
    REQUIRE(row.cpu_mask & Bit(vr::PerfStage::Geometry));
    vr::PerfEndFrame();
    vr::PerfResetForTests(); // releases the queries before their device
    context->Release(); device->Release();
}
// Consecutive sections, and stages past bit 31 of the 64-bit masks.
static void SectionsAndWideMask()
{
    Reset(); vr::PerfRequestStart(); vr::PerfBeginFrame(1); Scene();
    {
        vr::PerfSection perf;
        perf.Next(vr::PerfStage::PostAo);
        perf.Next(vr::PerfStage::LightAccum);
        perf.Next(vr::PerfStage::LightAccum); // the same part again adds up
    }
    vr::PerfEndFrame();
    const auto& row = Samples(1)[0];
    REQUIRE(unsigned(vr::PerfStage::LightAccum) >= 32);
    REQUIRE(row.cpu_mask == (Bit(vr::PerfStage::PostAo) | Bit(vr::PerfStage::LightAccum)));
    vr::PerfRequestStop(); vr::PerfEndFrame();
    bool header = false;
    for (const auto& line : lines)
        if (line.find("columns,") != std::string::npos)
            header = line.find(",gpu_light_accum_ms") != std::string::npos;
    REQUIRE(header);
}
// A full capture says so instead of looking idle, and a new start says what
// it discards.
static void UndumpedCaptureStatus()
{
    Reset(); vr::PerfRequestStart();
    for (unsigned i = 0; i < vr::PerfMaxFrames; ++i) { vr::PerfBeginFrame(i); vr::PerfEndFrame(); }
    vr::PerfBeginFrame(vr::PerfMaxFrames); vr::PerfEndFrame(); // full: recording stops
    lines.clear(); vr::PerfPrintStatus();
    REQUIRE(lines.size() == 1 && lines[0].find("not dumped") != std::string::npos);
    lines.clear(); vr::PerfRequestStart(); vr::PerfBeginFrame(0);
    REQUIRE(!lines.empty() && lines[0].find("discarding capture") != std::string::npos);
    vr::PerfEndFrame(); vr::PerfRequestStop(); vr::PerfEndFrame();
    lines.clear(); vr::PerfPrintStatus();
    REQUIRE(lines[0].find("idle/ready") != std::string::npos);
}
static void GpuLifecycleAndDump()
{
    Reset();
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    CreateWarp(device, context);
    vr::PerfAttach(device, context); vr::PerfRequestStart();
    for (unsigned i = 0; i < 32; ++i)
    {
        vr::PerfBeginFrame(i); Scene(); vr::PerfBeginGpuFrame();
        {
            vr::PerfScope eye(vr::PerfStage::LeftEye, true);
            vr::PerfScope geometry(vr::PerfStage::Geometry, true);
        }
        vr::PerfEndGpuFrame(); vr::PerfEndFrame();
    }
    // Exercises completed query retrieval as well as the bounded busy ring.
    FinishGpu(device, context);
    vr::PerfBeginFrame(33); Scene(); vr::PerfBeginGpuFrame();
    const auto* rows = Samples(33);
    unsigned completed = 0;
    for (unsigned i = 0; i < 32; ++i)
    {
        REQUIRE(rows[i].cpu_mask & Bit(vr::PerfStage::Geometry));
        REQUIRE(rows[i].gpu_state != vr::PerfGpuState::Error && rows[i].gpu_state != vr::PerfGpuState::Disjoint);
        if (rows[i].gpu_state == vr::PerfGpuState::Ready)
        {
            ++completed;
            REQUIRE(rows[i].gpu_total_ms >= 0);
            REQUIRE(rows[i].gpu_mask & Bit(vr::PerfStage::Geometry));
        }
        else REQUIRE(rows[i].gpu_total_ms < 0);
    }
    REQUIRE(completed > 0);
    // Device loss while a GPU scope is alive must not End a released query.
    {
        vr::PerfScope stale(vr::PerfStage::Scope, true);
        vr::PerfDetach();
    }
    vr::PerfEndFrame();
    REQUIRE(Samples(33)[32].aborted);
    REQUIRE(Samples(33)[32].gpu_mask == 0);
    vr::PerfRequestStop(); vr::PerfEndFrame();
    // Preserve real collector output for a parser end-to-end check.
    std::ofstream output("performance-test-capture.log");
    for (const auto& line : lines) output << line << '\n';
    output.close();
    REQUIRE(output.good());
    vr::PerfResetForTests();
    context->Release(); device->Release();
}
int main()
{
    DisabledAndCpuOnly();
    BoundaryAndWorkerSafety();
    CapacityAndFailures();
    UndumpedCaptureStatus();
    SectionsAndWideMask();
    GpuOverflowKeepsPartial();
    GpuLifecycleAndDump(); // last: writes the log the report test parses
    puts("PASS: bounded capture, missing-GPU semantics, stop boundaries, worker isolation, stale tokens, "
        "partial overflow, undumped status and WARP detach");
}
