#include <iostream>
#include <stdexcept>
#include "video_core/frame_trace.h"

void Check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

int main() try {
    using namespace VideoCore::FrameTrace;
    Check(!TracksGuestForMode(501, false) && TracksGuestForMode(501, true),
          "frame-chain capture still restricts guest IDs");
    for (const auto event : {Event::GpuSubmit, Event::DisplayComposeLong, Event::HostPresentLong,
                            Event::VsyncSignal, Event::GpuQueuePushBegin, Event::GpuWorkEnd,
                            Event::BufferDequeued, Event::BufferReleasedFrame,
                            Event::GuestHostCpuWindow, Event::BufferDequeueSignal}) {
        Check(EnabledForMode(event, 0, 0, true), "critical chain event filtered out");
        Check(!EnabledForMode(event, 0, 0, false), "diagnostic changed focused capture defaults");
    }
    for (const auto event : {Event::GuestThreadReady, Event::GuestDispatch, Event::GuestSvcBegin}) {
        Check(EnabledForMode(event, 501, 0, true) && !EnabledForMode(event, 501, 0, false),
              "new producer ID not tracked selectively");
    }
    for (const auto event : {Event::TextureUploadInfo, Event::GuestRunPc, Event::GuestSvcPc,
                            Event::GuestRunMemory, Event::PipelineTranslateLong})
        Check(!EnabledForMode(event, 0, 0, true), "capture enabled unnecessary expensive detail");
    std::cout << "PASS: frame chain tracks arbitrary guest IDs and boundaries without detailed draw tracing\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
