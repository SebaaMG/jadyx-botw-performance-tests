#include <iostream>
#include <stdexcept>
#include "video_core/renderer_d3d12/d3d12_partial_submit.h"
#include "video_core/frame_trace.h"

static void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int main() try {
    using D3D12::PartialSubmitPolicy;
    for (const auto event : {VideoCore::FrameTrace::Event::GpuFrameBoundary,
                            VideoCore::FrameTrace::Event::GpuListWork,
                            VideoCore::FrameTrace::Event::GpuListUpload,
                            VideoCore::FrameTrace::Event::GpuListClose,
                            VideoCore::FrameTrace::Event::GpuListReset,
                            VideoCore::FrameTrace::Event::GpuListPreviousSubmitted,
                            VideoCore::FrameTrace::Event::GpuListPreviousCompleted}) {
        Check(VideoCore::FrameTrace::EnabledForMode(event, 0, 0, true) &&
              !VideoCore::FrameTrace::EnabledForMode(event, 0, 0, false),
              "submission diagnostics escaped selective capture");
    }
    Check(VideoCore::FrameTrace::EnabledForMode(VideoCore::FrameTrace::Event::GpuPartialSubmit, 0, 0, true) &&
          !VideoCore::FrameTrace::EnabledForMode(VideoCore::FrameTrace::Event::GpuPartialSubmit, 0, 0, false),
          "partial submit marker escaped selective frame-chain capture");
    PartialSubmitPolicy policy;
    Check(policy.RecordWork() && !policy.Due(100), "first operation did not initialize the clock");
    for (unsigned work = 2; work <= 128; ++work) {
        const bool probe = policy.RecordWork();
        Check(probe == (work == 128), "clock probe outside a work boundary");
    }
    Check(!policy.Due(4099), "submitted before the latency budget");
    Check(policy.Due(4100), "long list was not eligible");
    Check(!policy.Due(99), "clock reversal caused a premature submission");
    policy.PartiallySubmitted();
    for (unsigned partial = 1; partial < PartialSubmitPolicy::MaxPerFrame; ++partial) {
        Check(policy.RecordWork() && !policy.Due(0), "new list did not restart its clock");
        for (unsigned work = 2; work <= 128; ++work) policy.RecordWork();
        Check(policy.Due(4000), "later partial submission did not become eligible");
        policy.PartiallySubmitted();
    }
    for (unsigned work = 0; work < 10000; ++work) {
        Check(!policy.RecordWork(), "frame limit did not bound partial submissions and probes");
    }
    policy.Submitted();
    Check(!policy.RecordWork(), "ordinary flush bypassed the frame limit");
    policy.BeginFrame();
    Check(policy.RecordWork() && !policy.Due(0), "next frame did not rearm");
    for (unsigned work = 2; work <= 128; ++work) policy.RecordWork();
    policy.Submitted();
    Check(policy.PendingWork() == 0, "ordinary submission retained old work");
    Check(policy.RecordWork() && !policy.Due(100000), "ordinary submission retained old latency");
    PartialSubmitPolicy continuous{true};
    for (unsigned delivery = 0; delivery < 512; ++delivery) {
        const std::uint64_t start = 100 + delivery * 10000;
        Check(continuous.RecordWork() && !continuous.Due(start), "continuous list did not start its clock");
        for (unsigned work = 2; work <= 128; ++work) {
            const bool probe = continuous.RecordWork();
            Check(probe == (work == 128), "continuous mode changed clock sampling");
        }
        Check(!continuous.Due(start + 1999), "drained queue bypassed the minimum interval");
        Check(continuous.Due(start + 2000), "drained queue did not get its shorter interval");
        Check(!continuous.Due(start + 2000, 1), "queued work bypassed its batching interval");
        Check(continuous.Due(start + 4000, 1), "queued work failed to become eligible");
        Check(!continuous.Due(start + 100000, 2), "full queue bypassed backpressure");
        Check(!continuous.Due(start - 1), "continuous clock reversal submitted early");
        continuous.PartiallySubmitted();
    }
    Check(continuous.RecordWork() && !continuous.Due(100), "continuous supply stopped without BeginFrame");
    continuous.BeginFrame();
    for (unsigned work = 2; work <= 128; ++work) continuous.RecordWork();
    Check(!continuous.Due(2099), "frame boundary restarted or bypassed the list interval");
    Check(continuous.Due(2100), "frame boundary prevented continuous supply");
    continuous.Submitted();
    Check(continuous.PendingWork() == 0, "natural flush retained continuous work");
    Check(continuous.RecordWork() && !continuous.Due(100000), "natural flush retained continuous latency");
    std::cout << "PASS: legacy frame cap, continuous 512 deliveries without frame reset, queue-aware intervals, backpressure and flush boundaries\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
