#define SERIESX_PARTIAL_SUBMIT_NO_MAIN
#include "partial-submit-gpu.cpp"
#undef SERIESX_PARTIAL_SUBMIT_NO_MAIN

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <thread>
#include "common/fs/path_util.h"
#include "common/scope_exit.h"
#include "core/core.h"
#include "core/cpu_manager.h"
#include "core/frontend/emu_window.h"
#include "core/frontend/graphics_context.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/kernel/k_readable_event.h"
#include "core/hle/kernel/k_scheduler.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/service/kernel_helpers.h"
#include "core/hle/service/nvdrv/core/container.h"
#include "core/hle/service/nvdrv/core/syncpoint_manager.h"
#include "core/hle/service/nvdrv/devices/nvhost_ctrl.h"
#include "core/hle/service/nvdrv/nvdrv.h"
#include "core/hle/service/nvnflinger/buffer_queue_consumer.h"
#include "core/hle/service/nvnflinger/buffer_queue_core.h"
#include "core/hle/service/nvnflinger/buffer_queue_producer.h"
#include "core/hle/service/nvnflinger/ui/graphic_buffer.h"
#include "core/hle/service/pulse_service.h"
#include "video_core/host1x/host1x.h"
#include "video_core/renderer_base.h"

using HostSyncpoints = Tegra::Host1x::SyncpointManager;
using namespace std::chrono_literals;

template <typename Predicate>
static bool Eventually(Predicate predicate, std::chrono::milliseconds timeout = 2000ms) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
        if (predicate()) return true;
        std::this_thread::sleep_for(1ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return predicate();
}

static void CheckActions(bool detached) {
    VideoCore::SetSynchronizationOptions({true, true, detached});
    HostSyncpoints manager;
    std::vector<u32> order;
    auto cancelled = manager.RegisterHostAction(1, 1, [&] { order.push_back(99); });
    manager.DeregisterHostAction(1, cancelled);
    manager.DeregisterHostAction(1, cancelled);
    auto first = manager.RegisterHostAction(1, 1, [&] { order.push_back(1); });
    manager.RegisterHostAction(1, 3, [&] { order.push_back(3); });
    manager.RegisterHostAction(1, 2, [&] { order.push_back(2); });
    for (u32 index = 0; index < 3; ++index) manager.IncrementHost(1);
    manager.DeregisterHostAction(1, first);
    manager.DeregisterHostAction(1, first);
    Check(order == std::vector<u32>{1, 2, 3}, "syncpoint ordering or stale cancellation failed");
    manager.RegisterHostAction(1, 2, [&] { order.push_back(4); });
    Check(order.back() == 4, "already completed syncpoint was not delivered inline");
    manager.RegisterGuestAction(2, 4, [&] { order.push_back(5); });
    manager.PulseRaise(2, 4, 0);
    Check(order.back() == 5, "PulseRaise lost a ready callback");
}

static void CheckUnrelatedProgress(bool detached) {
    VideoCore::SetSynchronizationOptions({true, true, detached});
    HostSyncpoints manager;
    std::atomic<bool> started{}, release{}, other_done{};
    manager.RegisterGuestAction(1, 1, [&] {
        started.store(true);
        while (!release.load()) std::this_thread::sleep_for(1ms);
    });
    std::jthread slow([&] { manager.IncrementGuest(1); });
    const bool began = Eventually([&] { return started.load(); });
    std::jthread unrelated([&] {
        manager.RegisterHostAction(2, 1, [&] { other_done.store(true); });
        manager.IncrementHost(2);
    });
    const bool progressed = Eventually([&] { return other_done.load(); }, detached ? 2000ms : 40ms);
    release.store(true);
    slow.join();
    unrelated.join();
    Check(began && progressed == detached, "callback retained unrelated syncpoint work");
}

static void CheckCancellationLifetime() {
    VideoCore::SetSynchronizationOptions({true, true, true});
    HostSyncpoints manager;
    std::atomic<bool> started{}, release{}, cancelled{}, cancelling{};
    auto action = manager.RegisterHostAction(1, 1, [&] {
        started.store(true);
        while (!release.load()) std::this_thread::sleep_for(1ms);
    });
    std::jthread signal([&] { manager.IncrementHost(1); });
    const bool began = Eventually([&] { return started.load(); });
    std::jthread cancel([&] {
        cancelling.store(true);
        manager.DeregisterHostAction(1, action);
        cancelled.store(true);
    });
    const bool entered = Eventually([&] { return cancelling.load(); });
    const bool returned_early = Eventually([&] { return cancelled.load(); }, 40ms);
    release.store(true);
    signal.join();
    cancel.join();
    Check(began && entered && !returned_early && cancelled.load(),
          "cancellation released a callback still using its owner");
    HostSyncpoints::ActionHandle recursive;
    recursive = manager.RegisterGuestAction(3, 1, [&] {
        manager.DeregisterGuestAction(3, recursive);
        manager.RegisterHostAction(4, 1, [&] { manager.IncrementGuest(5); });
        manager.IncrementHost(4);
    });
    manager.IncrementGuest(3);
    Check(manager.GetGuestSyncpointValue(5) == 1, "callback reentrancy deadlocked or lost work");
    std::atomic<bool> waiting{}, woke{}, invalid_callback{};
    manager.RegisterHostAction(7, 20, [&] { invalid_callback.store(true); });
    std::jthread waiter([&] {
        waiting.store(true);
        manager.WaitHost(7, 20);
        woke.store(true);
    });
    Check(Eventually([&] { return waiting.load(); }), "shutdown waiter did not start");
    manager.CancelWaits();
    waiter.join();
    manager.PulseRaise(7, 20, 20);
    Check(woke.load() && !invalid_callback.load(), "shutdown lost its waiter or ran a cancelled action");
}

static void CheckRaces() {
    VideoCore::SetSynchronizationOptions({true, true, true});
    HostSyncpoints manager;
    constexpr u32 Iterations = 2000;
    std::array<std::atomic<u32>, 4> calls{};
    std::vector<std::jthread> workers;
    for (u32 worker = 0; worker < calls.size(); ++worker) {
        workers.emplace_back([&, worker] {
            const u32 syncpoint = worker + 8;
            for (u32 target = 1; target <= Iterations; ++target) {
                manager.RegisterHostAction(syncpoint, target, [&, worker] { ++calls[worker]; });
                manager.IncrementHost(syncpoint);
            }
        });
    }
    for (auto& worker : workers) worker.join();
    for (const auto& count : calls) Check(count == Iterations, "concurrent stream lost a callback");
}

static void CheckNestedCancellation() {
    VideoCore::SetSynchronizationOptions({true, true, true});
    HostSyncpoints manager;
    HostSyncpoints::ActionHandle outer;
    bool nested{}, finished{};
    manager.RegisterHostAction(2, 1, [&] {
        manager.DeregisterGuestAction(1, outer);
        nested = true;
    });
    outer = manager.RegisterGuestAction(1, 1, [&] {
        manager.IncrementHost(2);
        finished = true;
    });
    manager.IncrementGuest(1);
    Check(nested && finished, "nested callback waited for an ancestor on its own thread");
    manager.DeregisterGuestAction(1, outer);
}

static void CheckReadySerialization(bool stop) {
    VideoCore::SetSynchronizationOptions({true, true, true});
    HostSyncpoints manager;
    std::atomic<bool> entered{}, release{}, registering{}, called{};
    manager.RegisterHostAction(1, 1, [&] {
        entered.store(true);
        while (!release.load()) std::this_thread::sleep_for(1ms);
    });
    std::jthread first([&] { manager.IncrementHost(1); });
    const bool started = Eventually([&] { return entered.load(); });
    std::jthread second([&] {
        registering.store(true);
        manager.RegisterHostAction(1, 1, [&] { called.store(true); });
    });
    const bool waiting = Eventually([&] { return registering.load(); });
    const bool overlapped = Eventually([&] { return called.load(); }, 40ms);
    if (stop) manager.CancelWaits();
    release.store(true);
    first.join();
    second.join();
    Check(started && waiting && !overlapped && called.load() == !stop,
          "ready callback bypassed queue serialization or ran after shutdown");
}

struct HostCounterMember {
    using Pointer = std::array<std::atomic<u32>, 192> HostSyncpoints::*;
    friend Pointer HostCounters(HostCounterMember);
};
template <auto Member> struct HostCounterAccess {
    friend HostCounterMember::Pointer HostCounters(HostCounterMember) { return Member; }
};
template struct HostCounterAccess<&HostSyncpoints::syncpoints_host>;

static void CheckCounterWrap(bool detached) {
    VideoCore::SetSynchronizationOptions({true, true, detached});
    HostSyncpoints manager;
    (manager.*HostCounters(HostCounterMember{}))[1].store(0xfffffffdu);
    std::vector<u32> results;
    for (u32 target : {0u, 1u, 0xffffffffu, 0xfffffffeu}) {
        manager.RegisterHostAction(1, target, [&, target] { results.push_back(target); });
    }
    Check(results.empty(), "wrapped future fence was incorrectly considered complete");
    for (u32 index = 0; index < 4; ++index) manager.IncrementHost(1);
    Check(results == std::vector<u32>{0xfffffffeu, 0xffffffffu, 0u, 1u},
          "wrapped syncpoint callbacks lost threshold ordering");
}

static void CheckOutOfOrderCompletions(bool detached) {
    VideoCore::SetSynchronizationOptions({true, true, detached});
    HostSyncpoints manager;
    std::vector<u32> order;
    manager.RegisterHostAction(15, 1, [&] { order.push_back(1); });
    manager.RegisterHostAction(15, 2, [&] { order.push_back(2); });
    manager.CompleteHost(15, 2);
    manager.CompleteGuest(15, 2);
    Check(manager.GetHostSyncpointValue(15) == 0 && manager.GetGuestSyncpointValue(15) == 0 &&
          order.empty(), "a later composite released an earlier unfinished framebuffer");
    manager.CompleteGuest(15, 1);
    Check(manager.GetGuestSyncpointValue(15) == 2 && manager.GetHostSyncpointValue(15) == 0,
          "recording completion incorrectly meant GPU completion");
    manager.CompleteHost(15, 1);
    manager.CompleteHost(15, 2);
    Check(manager.GetHostSyncpointValue(15) == 2 && order == std::vector<u32>{1, 2},
          "consumer completion lost its contiguous frontier or signalled twice");
}

class TestWindow final : public Core::Frontend::EmuWindow {
public:
    std::unique_ptr<Core::Frontend::GraphicsContext> CreateSharedContext() const override {
        return std::make_unique<Core::Frontend::GraphicsContext>();
    }
    bool IsShown() const override { return false; }
};

static std::filesystem::path WriteTestNro(const std::filesystem::path& root) {
    std::array<u8, 0x3000> image{};
    const auto write = [&](size_t offset, u32 value) {
        std::memcpy(image.data() + offset, &value, sizeof(value));
    };
    write(0, 0x14000020);
    write(8, 0x454d4f48);
    write(12, 0x57455242);
    write(0x10, 0x304f524e);
    write(0x18, static_cast<u32>(image.size()));
    write(0x20, 0);
    write(0x24, 0x1000);
    write(0x28, 0x1000);
    write(0x2c, 0x1000);
    write(0x30, 0x2000);
    write(0x34, 0x1000);
    write(0x38, 0x1000);
    write(0x80, 0x14000000);
    const auto file = root / "synchronization-test.nro";
    std::ofstream stream(file, std::ios::binary);
    stream.write(reinterpret_cast<const char*>(image.data()), image.size());
    Check(stream.good(), "could not write the original test NRO");
    return file;
}

struct CoreGate {
    TestWindow window;
    Core::System system;
    explicit CoreGate(const std::filesystem::path& path) {
        Settings::values.renderer_backend = Settings::RendererBackend::Null;
        Settings::values.use_asynchronous_gpu_emulation = false;
        system.Initialize();
        Service::AM::FrontendAppletParameters params{};
        params.applet_id = Service::AM::AppletId::Application;
        params.applet_type = Service::AM::AppletType::Application;
        params.launch_type = Service::AM::LaunchType::FrontendInitiated;
        const auto result = system.Load(window, path.string(), params);
        std::printf("CHECK: core Load returned %u\n", static_cast<u32>(result));
        Check(result == Core::SystemResultStatus::Success, "test NRO failed to initialize the real core");
        system.RegisterHostThread();
        std::puts("CHECK: core host thread registered");
        system.GPU().Start();
        system.GetCpuManager().OnGpuReady();
        system.Run();
        std::puts("CHECK: original test core resumed");
    }
    ~CoreGate() { system.ShutdownMainProcess(); }
};

using NvCtrl = Service::Nvidia::Devices::nvhost_ctrl;
using NvResult = Service::Nvidia::NvResult;

struct NvStatusMember {
    friend std::atomic<Service::Nvidia::EventState>& EventStatus(NvStatusMember, NvCtrl&, u32);
};
template <auto Member> struct NvStatusAccess {
    friend std::atomic<Service::Nvidia::EventState>& EventStatus(NvStatusMember,
                                                               NvCtrl& device, u32 slot) {
        return (device.*Member)[slot].status;
    }
};
template struct NvStatusAccess<&NvCtrl::events>;

static bool EventSignalled(Kernel::KEvent& event, Core::System& system) {
    Kernel::KScopedSchedulerLock lock{system.Kernel()};
    return event.GetReadableEvent().IsSignaled(system.Kernel());
}

template <size_t Count>
static NvResult CallNvIoctl(NvCtrl& device, u32 command, std::array<u32, Count>& params) {
    const auto input = params;
    Service::Nvidia::Ioctl ioctl{};
    ioctl.raw = command;
    return device.Ioctl1(0, ioctl,
        {reinterpret_cast<const u8*>(input.data()), sizeof(input)},
        {reinterpret_cast<u8*>(params.data()), sizeof(params)});
}

static void CheckNvEvents(Core::System& system) {
    using namespace Service::Nvidia;
    NvCore::Container container{system.Host1x()};
    std::puts("CHECK: nvhost test container created");
    Module module{system};
    std::puts("CHECK: nvhost test module created");
    EventInterface event_interface{module};
    NvCtrl device{system, event_interface, container};
    auto& counters = container.GetSyncpointManager();
    const u32 syncpoint = counters.AllocateSyncpoint(true);
    const u32 target = counters.IncrementSyncpointMaxExt(syncpoint, 1);
    std::array<u32, 1> registration{0};
    Check(CallNvIoctl(device, 0xc004001f, registration) == NvResult::Success, "event registration failed");
    Service::Pulse::StateWriter before;
    Check(device.PulseCapture(0, before), "could not snapshot idle event state");
    const auto start = std::chrono::steady_clock::now();
    for (u32 index = 0; index < 10000; ++index) {
        std::array<u32, 4> poll{syncpoint, target, 0, 0xffffffff};
        Check(CallNvIoctl(device, 0xc010001e, poll) == NvResult::Timeout && poll[3] == 0xffffffff,
              "pending zero-timeout poll waited, changed its input or rejected an unarmed slot");
    }
    std::array<u32, 4> allocation_poll{syncpoint, target, 0, 0xffffffff};
    Check(CallNvIoctl(device, 0xc010001d, allocation_poll) == NvResult::Timeout &&
          allocation_poll[3] == 0, "allocation poll allocated a ticket or returned a stale value");
    std::array<u32, 4> invalid_poll{Service::Nvidia::MaxSyncPoints, target, 0, 0xffffffff};
    Check(CallNvIoctl(device, 0xc010001e, invalid_poll) == NvResult::BadParameter,
          "invalid syncpoint escaped validation");
    Service::Pulse::StateWriter after;
    Check(device.PulseCapture(0, after) && before.bytes == after.bytes,
          "zero-timeout polling allocated, armed or changed an event");
    std::puts("CHECK: pending polls left event state unchanged");
    std::atomic<bool> gpu_held{}, release_gpu{}, request_returned{};
    std::jthread hold_gpu([&] {
        system.GPU().RunOnGpuThread([&] {
            gpu_held.store(true);
            while (!release_gpu.load()) std::this_thread::sleep_for(1ms);
        });
    });
    const bool held = Eventually([&] { return gpu_held.load(); });
    std::jthread request([&] {
        system.GPU().RequestFlushCommands();
        request_returned.store(true);
    });
    const bool nonblocking = Eventually([&] { return request_returned.load(); }, 40ms);
    release_gpu.store(true);
    request.join();
    hold_gpu.join();
    Check(held && nonblocking, "GPU progress notification blocked on unrelated GPU-thread work");
    for (u32 cycle = 0; cycle < 4; ++cycle) {
        std::array<u32, 4> wait{syncpoint, target, 1000, 0};
        Check(CallNvIoctl(device, 0xc010001e, wait) == NvResult::Timeout,
              "real wait stopped emulation after legitimate cancellation");
        std::array<u32, 1> clear{0};
        Check(CallNvIoctl(device, 0xc004001c, clear) == NvResult::Success, "event cancellation failed");
    }
    std::puts("CHECK: finite waits cancel and rearm without global recovery");
    std::array<u32, 4> wait{syncpoint, target, 0xffffffff, 0};
    Check(CallNvIoctl(device, 0xc010001e, wait) == NvResult::Timeout, "infinite wait did not arm an event");
    auto* event = device.PulseEvent(0);
    Check(event != nullptr, "registered kernel event disappeared");
    Check(!EventSignalled(*event, system), "pending wait signalled before completion");
    system.Host1x().GetSyncpointManager().IncrementHost(syncpoint);
    Check(EventSignalled(*event, system), "completed dependency did not signal its event");
    std::array<u32, 1> clear{0};
    Check(CallNvIoctl(device, 0xc004001c, clear) == NvResult::Success &&
          !EventSignalled(*event, system), "clear did not retire the completed event");
    std::array<u32, 4> completed{syncpoint, target, 0, 0xffffffff};
    Check(CallNvIoctl(device, 0xc010001e, completed) == NvResult::Success && completed[3] == target,
          "completed zero-timeout poll returned the wrong value");
    const u32 first_target = counters.IncrementSyncpointMaxExt(syncpoint, 1);
    std::array<u32, 4> old_wait{syncpoint, first_target, 1000, 0};
    Check(CallNvIoctl(device, 0xc010001e, old_wait) == NvResult::Timeout, "old generation did not arm");
    std::atomic<bool> entered{}, release{}, cleared{};
    system.Host1x().GetSyncpointManager().RegisterHostAction(syncpoint, first_target, [&] {
        entered.store(true);
        while (!release.load()) std::this_thread::sleep_for(1ms);
    });
    std::jthread signal([&] { system.Host1x().GetSyncpointManager().IncrementHost(syncpoint); });
    std::jthread cancel([&] {
        system.RegisterHostThread();
        if (!Eventually([&] { return entered.load(); })) return;
        std::array<u32, 1> cancel_params{0};
        cleared.store(CallNvIoctl(device, 0xc004001c, cancel_params) == NvResult::Success);
    });
    SCOPE_EXIT { release.store(true); };
    Check(Eventually([&] { return cleared.load(); }),
          "event cancellation waited for an unrelated callback");
    const u32 next_target = counters.IncrementSyncpointMaxExt(syncpoint, 1);
    std::array<u32, 4> new_wait{syncpoint, next_target, 1000, 0};
    Check(CallNvIoctl(device, 0xc010001e, new_wait) == NvResult::Timeout, "new generation did not arm");
    release.store(true);
    signal.join();
    cancel.join();
    auto* next_event = device.PulseEvent(0);
    Check(next_event != nullptr, "rearmed kernel event disappeared");
    Check(!EventSignalled(*next_event, system),
          "old callback signalled a later event generation");
    system.Host1x().GetSyncpointManager().IncrementHost(syncpoint);
    Check(EventSignalled(*next_event, system),
          "new event generation lost its completion");
    clear = {0};
    Check(CallNvIoctl(device, 0xc004001c, clear) == NvResult::Success,
          "could not retire the preceding generation");
    const u32 running_target = counters.IncrementSyncpointMaxExt(syncpoint, 1);
    std::array<u32, 4> running_wait{syncpoint, running_target, 1000, 0};
    Check(CallNvIoctl(device, 0xc010001e, running_wait) == NvResult::Timeout,
          "running cancellation did not arm");
    std::atomic<u32> ready{};
    std::atomic<bool> start_signal{}, start_cancel{}, cancel_returned{};
    std::jthread running_signal([&] {
        system.RegisterHostThread();
        ++ready;
        while (!start_signal.load()) std::this_thread::sleep_for(1ms);
        system.Host1x().GetSyncpointManager().IncrementHost(syncpoint);
    });
    std::jthread running_cancel([&] {
        system.RegisterHostThread();
        ++ready;
        while (!start_cancel.load()) std::this_thread::sleep_for(1ms);
        std::array<u32, 1> cancel_params{0};
        cancel_returned.store(CallNvIoctl(device, 0xc004001c, cancel_params) == NvResult::Success);
    });
    const bool threads_ready = Eventually([&] { return ready.load() == 2; });
    bool signalling{}, cancelling{}, returned_early{};
    {
        Kernel::KScopedSchedulerLock lock{system.Kernel()};
        start_signal.store(true);
        signalling = Eventually([&] {
            return EventStatus(NvStatusMember{}, device, 0).load() == EventState::Signalling;
        });
        start_cancel.store(true);
        cancelling = Eventually([&] {
            return EventStatus(NvStatusMember{}, device, 0).load() == EventState::Cancelling;
        });
        returned_early = Eventually([&] { return cancel_returned.load(); }, 40ms);
    }
    running_signal.join();
    running_cancel.join();
    Check(threads_ready && signalling && cancelling && !returned_early && cancel_returned.load() &&
          !EventSignalled(*next_event, system),
          "clear raced the kernel signal or left the retired event signalled");
    std::printf("PASS: 10000 real nvhost-ctrl polls and cancellation/rearm, elapsed %.3f ms\n",
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
}

struct SlotMember {
    using Pointer = Service::android::BufferQueueDefs::SlotsType Service::android::BufferQueueCore::*;
    friend Pointer CoreSlots(SlotMember);
};
template <auto Member> struct SlotAccess {
    friend SlotMember::Pointer CoreSlots(SlotMember) { return Member; }
};
template struct SlotAccess<&Service::android::BufferQueueCore::slots>;

struct CountMember {
    using Pointer = s32 Service::android::BufferQueueCore::*;
    friend Pointer CoreCount(CountMember);
};
template <auto Member> struct CountAccess {
    friend CountMember::Pointer CoreCount(CountMember) { return Member; }
};
template struct CountAccess<&Service::android::BufferQueueCore::override_max_buffer_count>;

struct ApiMember {
    using Pointer = Service::android::NativeWindowApi Service::android::BufferQueueCore::*;
    friend Pointer CoreApi(ApiMember);
};
template <auto Member> struct ApiAccess {
    friend ApiMember::Pointer CoreApi(ApiMember) { return Member; }
};
template struct ApiAccess<&Service::android::BufferQueueCore::connected_api>;

static Service::android::Fence CheckReturnedFence(Core::System& system, bool enabled,
    Service::Nvidia::NvFence consumer_fence = {3, 41}) {
    using namespace Service::android;
    VideoCore::SetSynchronizationOptions({true, enabled, true});
    Service::Nvidia::NvCore::Container container{system.Host1x()};
    Service::KernelHelpers::ServiceContext context{system, "sync-buffer-gate"};
    auto core = std::make_shared<BufferQueueCore>();
    BufferQueueConsumer consumer{core};
    BufferQueueProducer producer{context, core, container.GetNvMapFile()};
    core.get()->*CoreCount(CountMember{}) = 1;
    core.get()->*CoreApi(ApiMember{}) = NativeWindowApi::Egl;
    auto& slot = (core.get()->*CoreSlots(SlotMember{}))[0];
    slot.graphic_buffer = std::make_shared<GraphicBuffer>(4, 4, PixelFormat::Rgba8888, 0);
    slot.buffer_state = BufferState::Acquired;
    slot.frame_number = 7;
    slot.fence = Service::android::Fence::NoFence();
    slot.fence.num_fences = 1;
    slot.fence.fences[0] = {1, 23};
    auto release = Service::android::Fence::NoFence();
    release.num_fences = 1;
    release.fences[0] = consumer_fence;
    auto invalid = release;
    invalid.num_fences = 5;
    Check(consumer.ReleaseBuffer(0, 7, invalid) == Status::BadValue &&
          slot.buffer_state == BufferState::Acquired, "invalid fence count corrupted the slot");
    Check(consumer.ReleaseBuffer(0, 8, release) == Status::StaleBufferSlot &&
          slot.buffer_state == BufferState::Acquired, "stale release changed a newer slot");
    Check(consumer.ReleaseBuffer(0, 7, release) == Status::NoError, "valid buffer release failed");
    s32 selected = -1;
    auto returned = Service::android::Fence::NoFence();
    const auto result = producer.DequeueBuffer(&selected, &returned, false, 4, 4, PixelFormat::Rgba8888, 0);
    Check(static_cast<s32>(result) >= 0 && selected == 0 && returned.num_fences == 1 &&
          returned.fences[0].id == (enabled ? consumer_fence.id : 1) &&
          returned.fences[0].value == (enabled ? consumer_fence.value : 23), "dequeue returned the wrong release dependency");
    producer.CancelBuffer(selected, returned);
    return returned;
}

struct IdleCache {
    std::mutex mutex;
    void AccumulateFlushes() {}
    void CommitAsyncFlushes() {}
    void PopAsyncFlushes() {}
    bool HasUncommittedFlushes() const { return false; }
    bool ShouldWaitAsyncFlushes() const { return false; }
};
struct CompletionTraits {
    using FenceType = D3D12::Fence;
    using BufferCacheType = IdleCache;
    using TextureCacheType = IdleCache;
    using QueryCacheType = IdleCache;
    static constexpr bool HAS_ASYNC_CHECK = true;
};
class CompletionManager final : public VideoCommon::FenceManager<CompletionTraits> {
public:
    CompletionManager(VideoCore::RasterizerInterface& host, Tegra::GPU& gpu,
                      IdleCache& texture, IdleCache& buffer, IdleCache& query, Scheduler& scheduler_)
        : VideoCommon::FenceManager<CompletionTraits>{host, gpu, texture, buffer, query},
          scheduler{scheduler_} {}
protected:
    D3D12::Fence CreateFence(bool stub) override { return std::make_shared<InnerFence>(scheduler, stub); }
    void QueueFence(D3D12::Fence& fence) override { fence->Queue(); }
    bool IsFenceSignaled(D3D12::Fence& fence) const override { return fence->IsSignaled(); }
    void WaitFence(D3D12::Fence& fence) override { fence->Wait(); }
private:
    Scheduler& scheduler;
};

static void CheckPhysicalCompletion(Device& device, Core::System& system) {
    Service::Nvidia::NvCore::Container container{system.Host1x()};
    Service::Nvidia::Module module{system};
    Service::Nvidia::EventInterface events{module};
    NvCtrl nvctrl{system, events, container};
    auto& manager = system.Host1x().GetSyncpointManager();
    auto& guest_counters = container.GetSyncpointManager();
    u32 syncpoint{};
    do {
        syncpoint = guest_counters.AllocateSyncpoint(true);
    } while (manager.GetHostSyncpointValue(syncpoint) || manager.GetGuestSyncpointValue(syncpoint));
    const u32 target = guest_counters.IncrementSyncpointMaxExt(syncpoint, 1);
    Scheduler scheduler{device};
    IdleCache texture, buffer, query;
    CompletionManager completion{*system.GPU().Renderer().ReadRasterizer(), system.GPU(),
                                  texture, buffer, query, scheduler};
    auto source = CreateCommittedBuffer(device.Get(), 4096, D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE, "consumer source");
    auto snapshot = CreateCommittedBuffer(device.Get(), 4096, D3D12_HEAP_TYPE_READBACK,
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE, "consumer copy");
    void* mapped{};
    const D3D12_RANGE none{};
    ThrowIfFailed(source->Map(0, &none, &mapped), "map consumer source");
    for (u32 index = 0; index < 1024; ++index) static_cast<u32*>(mapped)[index] = index * 31 + 9;
    source->Unmap(0, nullptr);
    ComPtr<ID3D12Fence> blocked, display;
    ThrowIfFailed(device.Get()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&blocked)), "consumer blocker");
    ThrowIfFailed(device.Get()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&display)), "display blocker");
    struct Release {
        ID3D12Fence* blocked;
        ID3D12Fence* display;
        ~Release() { blocked->Signal(1); display->Signal(1); }
    } release{blocked.Get(), display.Get()};
    {
        std::scoped_lock lock{device.QueueMutex()};
        ThrowIfFailed(device.Queue()->Wait(blocked.Get(), 1), "hold consumer read");
    }
    scheduler.CommandList()->CopyBufferRegion(snapshot.Get(), 0, source.Get(), 0, 4096);
    const u64 tick = scheduler.CurrentTick();
    completion.SignalConsumerFence(syncpoint, target);
    manager.CompleteGuest(syncpoint, target);
    Check(scheduler.CurrentTick() == tick + 1, "consumer fence added a duplicate submission");
    scheduler.WaitSubmitted(tick);
    {
        std::scoped_lock lock{device.QueueMutex()};
        ThrowIfFailed(device.Queue()->Wait(display.Get(), 1), "hold private presentation copy");
    }
    Check(manager.GetGuestSyncpointValue(syncpoint) == target &&
          manager.GetHostSyncpointValue(syncpoint) == 0,
          "guest completion incorrectly meant the consumer GPU read had finished");
    const auto returned = CheckReturnedFence(system, true, {static_cast<s32>(syncpoint), target});
    std::array<u32, 4> poll{static_cast<u32>(returned.fences[0].id), returned.fences[0].value, 0, 0xffffffff};
    Check(CallNvIoctl(nvctrl, 0xc010001e, poll) == NvResult::Timeout,
          "the producer could reuse its buffer before the real GPU consumer completed");
    ThrowIfFailed(blocked->Signal(1), "release consumer read");
    Check(Eventually([&] { return manager.GetHostSyncpointValue(syncpoint) == target; }),
          "consumer completion waited for unrelated presentation work");
    Check(CallNvIoctl(nvctrl, 0xc010001e, poll) == NvResult::Success,
          "physical completion did not release the dependency returned by dequeue");
    const D3D12_RANGE all{0, 4096};
    ThrowIfFailed(snapshot->Map(0, &all, &mapped), "map consumed image");
    for (u32 index = 0; index < 1024; ++index)
        Check(static_cast<const u32*>(mapped)[index] == index * 31 + 9, "consumer image was reused early");
    snapshot->Unmap(0, &none);
    ThrowIfFailed(display->Signal(1), "release private display copy");
    scheduler.Finish();
}

int main(int argc, char**) try {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const auto root = std::filesystem::current_path() / (argc > 1 ? "sync-data-async" : "sync-data-inline");
    for (u32 index = 0; index <= static_cast<u32>(Common::FS::EdenPath::IconsDir); ++index) {
        const auto folder = root / std::to_string(index);
        std::filesystem::create_directories(folder);
        Common::FS::SetEdenPath(static_cast<Common::FS::EdenPath>(index), folder);
    }
    Common::Log::Initialize();
    std::atomic<bool> finished{};
    std::jthread watchdog([&] {
        for (u32 count = 0; count < 600 && !finished.load(); ++count) std::this_thread::sleep_for(100ms);
        if (!finished.load()) std::_Exit(3);
    });
    SCOPE_EXIT { finished.store(true); };
    for (bool detached : {false, true}) {
        std::printf("CHECK: actions and unrelated progress, detached=%d\n", detached);
        CheckActions(detached);
        CheckCounterWrap(detached);
        CheckOutOfOrderCompletions(detached);
        CheckUnrelatedProgress(detached);
    }
    std::puts("CHECK: cancellation lifetime and reentrancy");
    CheckCancellationLifetime();
    std::puts("CHECK: concurrent streams");
    CheckRaces();
    std::puts("CHECK: nested cancellation and already-ready serialization");
    CheckNestedCancellation();
    CheckReadySerialization(false);
    CheckReadySerialization(true);
    VideoCore::SetSynchronizationOptions({true, true, true});
    std::puts("CHECK: loading the original test NRO with the null renderer");
    CoreGate core{WriteTestNro(root)};
    CheckNvEvents(core.system);
    CheckReturnedFence(core.system, false);
    CheckReturnedFence(core.system, true);
    Settings::values.renderer_debug = true;
    Settings::values.gpu_accuracy = Settings::GpuAccuracy::Low;
    Settings::values.gpu_fence_behavior = Settings::GpuFenceBehavior::Immediate;
    SetAsyncRecording(argc > 1);
    Device device;
    ComPtr<ID3D12InfoQueue> debug;
    ThrowIfFailed(device.Get()->QueryInterface(IID_PPV_ARGS(&debug)), "consumer info queue");
    debug->ClearStoredMessages();
    std::puts("CHECK: blocked native queue and consumer completion");
    CheckPhysicalCompletion(device, core.system);
    DebugClean(debug.Get());
    std::puts("PASS: nvhost events, stable syncpoint actions and physical consumer completion; D3D12 debug clean");
    return 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
}
