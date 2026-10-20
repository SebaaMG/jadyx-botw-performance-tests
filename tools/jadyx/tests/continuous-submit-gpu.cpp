#define SERIESX_PARTIAL_SUBMIT_NO_MAIN
#include "partial-submit-gpu.cpp"
#undef SERIESX_PARTIAL_SUBMIT_NO_MAIN

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <utility>
#include "core/core.h"
#include "core/device_memory.h"
#include "video_core/memory_manager.h"
#include "video_core/renderer_null/null_rasterizer.h"
#include "video_core/renderer_d3d12/d3d12_conditional_rendering.h"
#include "video_core/renderer_d3d12/d3d12_query_cache.h"

using NativeQueryBase = VideoCommon::QueryCacheLegacy<QueryCache, CachedQuery, CounterStream, HostCounter>;

struct QueryRegisterMember {
    using Pointer = CachedQuery* (NativeQueryBase::*)(VideoCore::QueryType, VAddr, u8*, bool);
    friend Pointer QueryGateMember(QueryRegisterMember);
};

template <typename Tag, auto Member>
struct QueryGateAccess {
    friend typename Tag::Pointer QueryGateMember(Tag) { return Member; }
};

template struct QueryGateAccess<QueryRegisterMember, &NativeQueryBase::Register>;

struct QueryGateMemory {
    static constexpr GPUVAddr GpuAddress = 0x10000;
    static constexpr DAddr DeviceAddress = 0x1000;
    Core::System system;
    Tegra::GPU gpu{system, false, false};
    Null::RasterizerNull host{gpu};
    Core::DeviceMemory physical;
    Tegra::MaxwellDeviceMemoryManager device_memory{physical};
    Tegra::MemoryManager gpu_memory{system, device_memory, 32, 0, 16, 12};
    std::array<u64, 512> reports{};

    QueryGateMemory() {
        device_memory.BindInterface(&host);
        gpu_memory.BindRasterizer(&host);
        Check(gpu_memory.Map(GpuAddress, DeviceAddress, 4096, Tegra::PTEKind::INVALID, false) == GpuAddress,
              "query gate address mapping changed");
    }

    template <typename Cache = QueryCache>
    std::unique_ptr<Cache> CreateQueries(Device& device, Scheduler& scheduler) {
        static_assert(std::is_constructible_v<Cache, VideoCore::RasterizerInterface&,
                          Tegra::MaxwellDeviceMemoryManager&, const Device&, Scheduler&>,
                      "The real QueryCache needs a RasterizerInterface constructor overload for this gate");
        return std::make_unique<Cache>(static_cast<VideoCore::RasterizerInterface&>(host),
                                       device_memory, device, scheduler);
    }

    GPUVAddr Register(QueryCache& cache, u32 word, std::shared_ptr<HostCounter> counter) {
        Check(word < reports.size() && word % 2 == 0, "invalid query report fixture offset");
        const auto device_address = DeviceAddress + word * sizeof(u64);
        Check(!cache.VisitQuery(device_address, [](CachedQuery* query) { return query != nullptr; }),
              "query report fixture reused a live destination");
        auto* query = (cache.*QueryGateMember(QueryRegisterMember{}))(
            VideoCore::QueryType::SamplesPassed, device_address,
            reinterpret_cast<u8*>(&reports[word]), false);
        Check(!query->BindCounter(std::move(counter), std::nullopt).has_value(),
              "fresh report unexpectedly read back an older counter");
        return GpuAddress + word * sizeof(u64);
    }
};

struct QueryGateDrain {
    Scheduler& scheduler;
    ~QueryGateDrain() { scheduler.DrainForShutdown(); }
};

class QueryQueueHold {
public:
    explicit QueryQueueHold(Device& device) {
        ThrowIfFailed(device.Get()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)),
                      "query gate blocker");
        {
            std::scoped_lock lock{device.QueueMutex()};
            ThrowIfFailed(device.Queue()->Wait(fence.Get(), 1), "hold query gate queue");
        }
        watchdog = std::jthread([this] {
            std::unique_lock lock{mutex};
            if (!condition.wait_for(lock, std::chrono::seconds(10), [this] { return released; })) {
                timed_out.store(true, std::memory_order_release);
                fence->Signal(1);
            }
        });
    }

    ~QueryQueueHold() { Release(); }

    void Release() noexcept {
        {
            std::scoped_lock lock{mutex};
            released = true;
            fence->Signal(1);
        }
        condition.notify_all();
        if (watchdog.joinable()) watchdog.join();
    }

    void CheckResponsive() const {
        Check(!timed_out.load(std::memory_order_acquire),
              "constructing or folding an unresolved query chain waited for GPU completion");
    }

private:
    ComPtr<ID3D12Fence> fence;
    std::mutex mutex;
    std::condition_variable condition;
    bool released{};
    std::atomic<bool> timed_out{};
    std::jthread watchdog;
};

class QueryDrawSurface {
public:
    static constexpr u32 Words = 1024;

    QueryDrawSurface(Device& device, Scheduler& scheduler_, ID3DBlob* vertex)
        : scheduler{scheduler_} {
        const char pixel_shader[] =
            "RWByteAddressBuffer result:register(u0);"
            "cbuffer Push:register(b0){uint offset;uint value;};"
            "void main(float4 position:SV_Position){result.Store(offset,value);}";
        ComPtr<ID3DBlob> pixel, errors, serialized;
        ThrowIfFailed(D3DCompile(pixel_shader, sizeof(pixel_shader), nullptr, nullptr, nullptr,
                                "main", "ps_5_1", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &pixel, &errors),
                      "query gate pixel shader");
        std::array<D3D12_ROOT_PARAMETER, 2> parameters{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[0].Constants = {0, 0, 2};
        parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        parameters[1].Descriptor = {0, 0};
        parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        const D3D12_ROOT_SIGNATURE_DESC signature{2, parameters.data(), 0, nullptr,
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
        ThrowIfFailed(D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1,
                                                 &serialized, &errors), "query gate root serialization");
        ThrowIfFailed(device.Get()->CreateRootSignature(0, serialized->GetBufferPointer(),
            serialized->GetBufferSize(), IID_PPV_ARGS(&root)), "query gate root signature");
        D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
        description.pRootSignature = root.Get();
        description.VS = {vertex->GetBufferPointer(), vertex->GetBufferSize()};
        description.PS = {pixel->GetBufferPointer(), pixel->GetBufferSize()};
        description.SampleMask = ~0u;
        description.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        description.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        description.RasterizerState.DepthClipEnable = TRUE;
        description.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        description.SampleDesc.Count = 1;
        for (auto& target : description.BlendState.RenderTarget) {
            target.SrcBlend = target.DestBlend = D3D12_BLEND_ONE;
            target.SrcBlendAlpha = target.DestBlendAlpha = D3D12_BLEND_ONE;
            target.BlendOp = target.BlendOpAlpha = D3D12_BLEND_OP_ADD;
            target.LogicOp = D3D12_LOGIC_OP_NOOP;
            target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        }
        ThrowIfFailed(device.Get()->CreateGraphicsPipelineState(&description, IID_PPV_ARGS(&pipeline)),
                      "query gate pipeline");
        upload = CreateCommittedBuffer(device.Get(), Words * sizeof(u32), D3D12_HEAP_TYPE_UPLOAD,
            D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE, "query gate zeros");
        output = CreateCommittedBuffer(device.Get(), Words * sizeof(u32), D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
            "query gate output");
        readback = CreateCommittedBuffer(device.Get(), Words * sizeof(u32), D3D12_HEAP_TYPE_READBACK,
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE, "query gate readback");
        void* mapped{};
        const D3D12_RANGE empty{};
        ThrowIfFailed(upload->Map(0, &empty, &mapped), "map query gate zeros");
        std::memset(mapped, 0, Words * sizeof(u32));
        upload->Unmap(0, nullptr);
        scheduler.CommandList()->CopyBufferRegion(output.Get(), 0, upload.Get(), 0, Words * sizeof(u32));
        TransitionResource(scheduler.CommandList(), output.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    void Draw(u32 word, u32 value, bool visible = true) {
        Check(word < Words, "query gate draw offset overflow");
        auto* commands = scheduler.CommandList();
        commands->SetGraphicsRootSignature(root.Get());
        scheduler.SetPipelineState(pipeline.Get());
        const D3D12_VIEWPORT viewport{0, 0, 1, 1, 0, 1};
        const D3D12_RECT scissor{0, 0, visible ? 1 : 0, visible ? 1 : 0};
        commands->RSSetViewports(1, &viewport);
        commands->RSSetScissorRects(1, &scissor);
        commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        commands->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
        const std::array<u32, 2> constants{word * static_cast<u32>(sizeof(u32)), value};
        commands->SetGraphicsRoot32BitConstants(0, 2, constants.data(), 0);
        const auto address = output->GetGPUVirtualAddress();
        commands->Record([address](ID3D12GraphicsCommandList* list) {
            list->SetGraphicsRootUnorderedAccessView(1, address);
        });
        commands->DrawInstanced(3, 1, 0, 0);
        const D3D12_RESOURCE_BARRIER barrier{.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV,
                                             .UAV = {.pResource = output.Get()}};
        commands->ResourceBarrier(1, &barrier);
    }

    void CheckWords(const std::array<u32, Words>& expected) {
        TransitionResource(scheduler.CommandList(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                           D3D12_RESOURCE_STATE_COPY_SOURCE);
        scheduler.CommandList()->CopyBufferRegion(readback.Get(), 0, output.Get(), 0, Words * sizeof(u32));
        scheduler.Finish();
        void* mapped{};
        const D3D12_RANGE range{0, Words * sizeof(u32)}, empty{};
        ThrowIfFailed(readback->Map(0, &range, &mapped), "map query gate result");
        const bool exact = std::memcmp(mapped, expected.data(), Words * sizeof(u32)) == 0;
        readback->Unmap(0, &empty);
        Check(exact, "query or conditional rendering changed exact GPU output");
    }

private:
    Scheduler& scheduler;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12Resource> upload, output, readback;
};

static void CheckPersistentQueries(Device& device, QueryGateMemory& memory, ID3DBlob* vertex) {
    SetPartialSubmission(true);
    SetContinuousSubmission(true);
    Scheduler scheduler{device};
    QueryDrawSurface surface{device, scheduler, vertex};
    auto queries = memory.CreateQueries(device, scheduler);
    QueryGateDrain drain{scheduler};
    std::array<u32, QueryDrawSurface::Words> expected{};
    constexpr u32 Batches = 112, BatchDraws = 128;
    std::shared_ptr<HostCounter> snapshot;
    for (u32 batch = 0; batch < Batches; ++batch) {
        for (u32 draw = 0; draw < BatchDraws; ++draw) {
            const u32 index = batch * BatchDraws + draw;
            const u32 word = index % QueryDrawSurface::Words;
            surface.Draw(word, index + 1);
            expected[word] = index + 1;
            if (draw + 1 == BatchDraws) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            u32 pending = 1;
            PartialSubmitScope boundary{scheduler, pending};
            boundary.Recorded();
        }
        const u64 submitted = scheduler.CurrentTick() - 1;
        scheduler.WaitSubmitted(submitted);
        scheduler.Wait(submitted);
        if (batch == 7) snapshot = queries->Stream(VideoCore::QueryType::SamplesPassed).Current();
        if (batch == 31 || batch == 79) {
            InnerFence fence{scheduler, false};
            bool notified{};
            fence.Queue([&] { notified = true; });
            Check(notified, "real fence did not acknowledge the query boundary");
        }
    }
    Check(scheduler.ReadSubmissionStatistics().partial_submits > 96,
          "persistent query gate did not cross 96 continuous submissions");
    auto total = queries->Stream(VideoCore::QueryType::SamplesPassed).Current();
    auto primitives = queries->Stream(VideoCore::QueryType::PrimitivesGenerated).Current();
    auto streamed = queries->Stream(VideoCore::QueryType::TfbPrimitivesWritten).Current();
    Check(total && total->Depth() <= 2,
          "completed query prefixes did not bound the remaining dependency depth");
    memory.Register(*queries, 64, snapshot);
    memory.Register(*queries, 66, total);
    surface.CheckWords(expected);
    const auto old_report = queries->PeekReport(QueryGateMemory::DeviceAddress + 64 * sizeof(u64));
    const auto new_report = queries->PeekReport(QueryGateMemory::DeviceAddress + 66 * sizeof(u64));
    Check(old_report && new_report && old_report->complete && new_report->complete &&
          old_report->known == 8ULL * BatchDraws && new_report->known == u64(Batches) * BatchDraws,
          "folding the current report changed a shared earlier report");
    Check(total && total->Query() == u64(Batches) * BatchDraws,
          "production persistent occlusion total changed across repeated cuts");
    Check(snapshot && snapshot->Query() == 8ULL * BatchDraws,
          "folding a live stream changed a shared earlier snapshot");
    Check(primitives && primitives->Query() == u64(Batches) * BatchDraws,
          "production primitive counter lost a submitted slice");
    Check(streamed && streamed->Query() == 0, "unused stream-output counter changed");
    std::puts("PASS: production QueryCache streams cross 96 continuous submissions with exact shared totals");
}

static void CheckBlockedQueryChain(Device& device, QueryGateMemory& memory, ID3DBlob* vertex,
                                  bool destroy_while_blocked) {
    SetPartialSubmission(true);
    SetContinuousSubmission(true);
    Scheduler scheduler{device};
    QueryDrawSurface surface{device, scheduler, vertex};
    auto queries = memory.CreateQueries(device, scheduler);
    QueryGateDrain drain{scheduler};
    QueryQueueHold hold{device};
    constexpr u32 Slices = 160;
    std::array<u32, QueryDrawSurface::Words> expected{};
    std::shared_ptr<HostCounter> snapshot;
    for (u32 slice = 0; slice < Slices; ++slice) {
        surface.Draw(slice, slice + 1);
        expected[slice] = slice + 1;
        if (slice == 39) snapshot = queries->Stream(VideoCore::QueryType::SamplesPassed).Current();
        scheduler.Flush();
        hold.CheckResponsive();
    }
    auto total = queries->Stream(VideoCore::QueryType::SamplesPassed).Current();
    Check(total && total->Depth() > 96 && !total->IsKnownComplete(),
          "uncompleted counter dependencies were folded or forcibly read back");
    total->FoldCompletedDependencies();
    hold.CheckResponsive();
    Check(total->Depth() > 96 && !total->KnownResult(),
          "nonblocking fold consumed an unresolved GPU result");
    if (destroy_while_blocked) {
        const std::weak_ptr<HostCounter> dropped = total;
        total.reset();
        snapshot.reset();
        queries->ResetCounter(VideoCore::QueryType::SamplesPassed);
        queries->ResetCounter(VideoCore::QueryType::PrimitivesGenerated);
        queries->ResetCounter(VideoCore::QueryType::TfbPrimitivesWritten);
        Check(dropped.expired(), "reset retained a unique unresolved counter chain");
        hold.CheckResponsive();
    }
    hold.Release();
    surface.CheckWords(expected);
    if (!destroy_while_blocked) {
        Check(total->Query() == Slices, "iterative query lost an unresolved slice");
        Check(snapshot && snapshot->Query() == 40, "iterative query changed a shared snapshot result");
        Check(total->KnownResult() && snapshot->KnownResult(), "iterative query did not cache shared totals");
        Check(total->Query() == Slices && snapshot->Query() == 40,
              "cached iterative query double counted a dependency");
    }
    std::puts(destroy_while_blocked
        ? "PASS: unique production chains deeper than 96 are released without waiting"
        : "PASS: 160 blocked production slices remain nonblocking and preserve shared query totals");
}

static void SetCondition(Tegra::Engines::Maxwell3D::Regs& registers, GPUVAddr address,
                         Tegra::Engines::Maxwell3D::Regs::RenderEnable::Mode mode) {
    using RenderEnable = Tegra::Engines::Maxwell3D::Regs::RenderEnable;
    registers.render_enable_override = RenderEnable::Override::UseRenderEnable;
    registers.render_enable.address_high = static_cast<u32>(address >> 32);
    registers.render_enable.address_low = static_cast<u32>(address);
    registers.render_enable.mode = mode;
}

static void CheckConditionalQueries(Device& device, QueryGateMemory& memory, ID3DBlob* vertex) {
    SetPartialSubmission(true);
    SetContinuousSubmission(true);
    Scheduler scheduler{device};
    QueryDrawSurface surface{device, scheduler, vertex};
    auto queries = memory.CreateQueries(device, scheduler);
    ConditionalRendering conditional{device, scheduler, *queries};
    QueryGateDrain drain{scheduler};
    QueryQueueHold hold{device};
    using QueryType = VideoCore::QueryType;
    using RenderEnable = Tegra::Engines::Maxwell3D::Regs::RenderEnable;
    auto registers = std::make_unique<Tegra::Engines::Maxwell3D::Regs>();
    std::array<u32, QueryDrawSurface::Words> expected{};
    auto zero = queries->Stream(QueryType::SamplesPassed).Current();
    const auto zero_address = memory.Register(*queries, 0, zero);
    queries->ResetCounter(QueryType::SamplesPassed);
    surface.Draw(0, 17);
    expected[0] = 17;
    auto one = queries->Stream(QueryType::SamplesPassed).Current();
    const auto one_address = memory.Register(*queries, 2, one);
    const auto same_address = memory.Register(*queries, 4, one);
    scheduler.Flush();

    SetCondition(*registers, one_address, RenderEnable::Mode::Conditional);
    Check(conditional.Accelerate(*registers, memory.gpu_memory) && !conditional.SkipsDraws(),
          "single unresolved production slice did not enable GPU predication");
    {
        ConditionalRendering::Scope predicate{conditional, scheduler.CommandList()};
        surface.Draw(1, 23);
    }
    expected[1] = 23;
    scheduler.Flush();
    {
        ConditionalRendering::Scope predicate{conditional, scheduler.CommandList()};
        surface.Draw(2, 29);
    }
    expected[2] = 29;
    SetCondition(*registers, zero_address, RenderEnable::Mode::Conditional);
    Check(conditional.Accelerate(*registers, memory.gpu_memory), "zero slice did not select a predicate");
    {
        ConditionalRendering::Scope predicate{conditional, scheduler.CommandList()};
        surface.Draw(3, 31);
    }
    SetCondition(*registers, one_address, RenderEnable::Mode::IfEqual);
    Check(conditional.Accelerate(*registers, memory.gpu_memory) && !conditional.SkipsDraws(),
          "shared production reports did not compare equal");
    surface.Draw(4, 37);
    expected[4] = 37;
    SetCondition(*registers, one_address, RenderEnable::Mode::IfNotEqual);
    Check(conditional.Accelerate(*registers, memory.gpu_memory) && conditional.SkipsDraws(),
          "shared production reports did not suppress an unequal comparison");
    Check(same_address == one_address + 16, "comparison report fixture is not adjacent");

    std::shared_ptr<HostCounter> many;
    for (u32 slice = 0; slice < 8; ++slice) {
        surface.Draw(32 + slice, 41 + slice);
        expected[32 + slice] = 41 + slice;
        scheduler.Flush();
        hold.CheckResponsive();
    }
    many = queries->Stream(QueryType::SamplesPassed).Current();
    const auto many_address = memory.Register(*queries, 8, many);
    const auto limited = queries->PeekReport(QueryGateMemory::DeviceAddress + 8 * sizeof(u64));
    Check(limited && !limited->complete && limited->num_slices == PendingReport::MAX_SLICES,
          "production PendingReport did not preserve its unresolved slice limit");
    SetCondition(*registers, many_address, RenderEnable::Mode::Conditional);
    Check(!conditional.Accelerate(*registers, memory.gpu_memory),
          "incomplete production query report was treated as a complete GPU predicate");
    hold.CheckResponsive();
    scheduler.Flush();
    hold.Release();
    scheduler.Wait(scheduler.CurrentTick() - 1);
    const auto folded = queries->PeekReport(QueryGateMemory::DeviceAddress + 8 * sizeof(u64));
    Check(folded && folded->complete && folded->num_slices == 0 && folded->known == 12,
          "completed production report did not fold its exact total");
    SetCondition(*registers, many_address, RenderEnable::Mode::Conditional);
    Check(conditional.Accelerate(*registers, memory.gpu_memory) && !conditional.SkipsDraws(),
          "completed query report failed to return to accelerated evaluation");
    surface.CheckWords(expected);
    Check(zero->Query() == 0 && one->Query() == 1 && many->Query() == 12,
          "conditional rendering changed shared production query values");
    std::puts("PASS: real QueryCache PendingReport limits, production GPU predicates and completed folding");
}

static void CheckCompletedPrefix(Device& device, QueryGateMemory& memory, ID3DBlob* vertex) {
    SetPartialSubmission(true);
    SetContinuousSubmission(true);
    Scheduler scheduler{device};
    QueryDrawSurface surface{device, scheduler, vertex};
    auto queries = memory.CreateQueries(device, scheduler);
    QueryGateDrain drain{scheduler};
    std::array<u32, QueryDrawSurface::Words> expected{};
    surface.Draw(0, 71);
    expected[0] = 71;
    auto prefix = queries->Stream(VideoCore::QueryType::SamplesPassed).Current();
    const u64 prefix_tick = scheduler.Flush();
    scheduler.Wait(prefix_tick);
    QueryQueueHold hold{device};
    surface.Draw(1, 73);
    expected[1] = 73;
    scheduler.Flush();
    auto latest = queries->Stream(VideoCore::QueryType::SamplesPassed).Current();
    Check(latest && !latest->IsKnownComplete(), "completed-prefix gate did not retain an unresolved head");
    latest->FoldCompletedDependencies();
    hold.CheckResponsive();
    Check(prefix->KnownResult() && *prefix->KnownResult() == 1,
          "ready prefix was not folded behind the unresolved newest slice");
    Check(latest->Depth() <= 2 && !latest->KnownResult(),
          "ready-prefix folding either retained old depth or consumed the unresolved head");
    hold.Release();
    surface.CheckWords(expected);
    Check(latest->Query() == 2 && prefix->Query() == 1,
          "completed-prefix folding lost or duplicated the shared total");
    std::puts("PASS: completed older prefix folds while the latest production slice remains blocked");
}

class ReentrantQueryCounter final : public VideoCommon::HostCounterBase<void, ReentrantQueryCounter> {
public:
    ReentrantQueryCounter(std::shared_ptr<ReentrantQueryCounter> dependency, u64 value_)
        : HostCounterBase{std::move(dependency), true}, value{value_} {}
    std::function<void()> before_read;
    u32 reads{};
    void FoldReady() { FoldReadyDependencies([](const ReentrantQueryCounter&) { return true; }); }

private:
    u64 BlockingQuery(bool) const override {
        auto& self = *const_cast<ReentrantQueryCounter*>(this);
        ++self.reads;
        if (auto callback = std::exchange(self.before_read, {})) callback();
        return value;
    }
    u64 value;
};

static void CheckReentrantQueryResults() {
    auto oldest = std::make_shared<ReentrantQueryCounter>(nullptr, 7);
    auto middle = std::make_shared<ReentrantQueryCounter>(oldest, 11);
    auto root = std::make_shared<ReentrantQueryCounter>(middle, 13);
    root->before_read = [&] { Check(root->Query() == 31, "inner query did not cache the inclusive result"); };
    Check(root->Query() == 31 && middle->Query() == 18 && oldest->Query() == 7,
          "outer blocking read overwrote a reentrantly cached inclusive result");
    Check(root->reads == 2 && middle->reads == 1 && oldest->reads == 1,
          "reentrant query reread an already cached dependency");
    auto prefix = std::make_shared<ReentrantQueryCounter>(nullptr, 19);
    auto active = std::make_shared<ReentrantQueryCounter>(prefix, 23);
    active->before_read = [&] { active->FoldReady(); };
    Check(active->Query() == 42 && prefix->Query() == 19,
          "query read its base before a reentrant completed-prefix fold");
    std::puts("PASS: reentrant inclusive-result caching and base-result mutation preserve exact totals");
}

int main(int argc, char**) try {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Common::Log::Initialize();
    Settings::values.renderer_debug = true;
    const bool worker = argc > 1;
    SetAsyncRecording(worker);
    Device device;
    ComPtr<ID3D12InfoQueue> debug;
    ThrowIfFailed(device.Get()->QueryInterface(IID_PPV_ARGS(&debug)), "continuous info queue");
    debug->ClearStoredMessages();
    const std::string shader =
        "cbuffer A0:register(b0){uint4 a0[32];}; cbuffer A1:register(b1){uint4 a1[32];};"
        "cbuffer A2:register(b2){uint4 a2[32];}; cbuffer A3:register(b3){uint4 a3[32];};"
        "cbuffer A4:register(b4){uint4 a4[32];}; cbuffer A5:register(b5){uint4 a5[32];};"
        "cbuffer A6:register(b6){uint4 a6[32];}; cbuffer A7:register(b7){uint4 a7[32];};"
        "cbuffer Push:register(b0,space" + std::to_string(PUSH_CONSTANT_SPACE) + "){uint index;};"
        "RWBuffer<uint4> result:register(u8); void main(float4 position:SV_Position){"
        "result[index]=a0[0]+a1[0]+a2[0]+a3[0]+a4[0]+a5[0]+a6[0]+a7[0]+a6[16];}";
    const char vertex_shader[] = "float4 main(uint index:SV_VertexID):SV_Position{"
        "return float4(index==1?3:-1,index==2?3:-1,0,1);}";
    const char query_vertex_shader[] = "float4 main(uint index:SV_VertexID):SV_Position{"
        "return float4(index==1?1:index==2?0:-1,index==2?1:-1,0,1);}";
    ComPtr<ID3DBlob> pixel, vertex, query_vertex, errors;
    ThrowIfFailed(D3DCompile(shader.data(), shader.size(), nullptr, nullptr, nullptr, "main", "ps_5_1",
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &pixel, &errors), "continuous PS");
    ThrowIfFailed(D3DCompile(vertex_shader, sizeof(vertex_shader), nullptr, nullptr, nullptr, "main", "vs_5_1",
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vertex, &errors), "continuous VS");
    ThrowIfFailed(D3DCompile(query_vertex_shader, sizeof(query_vertex_shader), nullptr, nullptr, nullptr,
                            "main", "vs_5_1", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &query_vertex, &errors),
                  "continuous query VS");
    CheckReentrantQueryResults();
    CheckBackpressure(device, true);
    CheckBoundaries(device, true);
    for (u32 pattern : {0u, 1u, 2u, 3u})
        for (bool partial : {false, true})
            CheckDraws(device, pixel.Get(), vertex.Get(), worker, partial, pattern, true);
    for (bool coalesce : {false, true}) CheckFenceBoundaries(device, coalesce);
    CheckFenceFailure(device);
    {
        std::puts("CHECK: constructing production query memory fixture");
        QueryGateMemory memory;
        CheckPersistentQueries(device, memory, query_vertex.Get());
        CheckBlockedQueryChain(device, memory, query_vertex.Get(), false);
        CheckBlockedQueryChain(device, memory, query_vertex.Get(), true);
        CheckConditionalQueries(device, memory, query_vertex.Get());
        CheckCompletedPrefix(device, memory, query_vertex.Get());
    }
    DebugClean(debug.Get());
    std::puts("PASS: continuous production queries, predicates, fences and reentrant folds; D3D12 debug clean");
    return 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
}
