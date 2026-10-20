#define main OriginalDepthGate
#include "tools/xbox/tests/depth-region-copy.cpp"
#undef main
#include <filesystem>
#include "common/fs/path_util.h"
#include "video_core/frame_trace.h"

int main(int argc, char** argv) try {
    namespace Trace = VideoCore::FrameTrace;
    const auto folder = std::filesystem::current_path() / (argc > 1 ? "trace-async" : "trace-inline");
    std::filesystem::create_directories(folder);
    Common::FS::SetEdenPath(Common::FS::EdenPath::LogDir, folder);
    Common::Log::Initialize();
    Settings::values.renderer_debug = true;
    SetAsyncRecording(argc > 1 && std::string{argv[1]} == "--async");
    Gate gate;
    const auto expected = Pattern(65, 47, 19);
    auto image = gate.Make(PixelFormat::A8B8G8R8_UNORM, expected);
    gate.scheduler.Finish();
    const auto layout = WholeLevel(expected.width, expected.height);
    auto readback = gate.runtime.DownloadStagingBuffer(layout.buffer_size, true);
    Trace::SetFrameChainMode(true);
    Trace::Start(480);
    Check(Trace::GetCaptureStatus().vsyncs_remaining == 120, "frame trace was not bounded");
    {
        const Trace::ScopedRequest request{Trace::Event::IpcRequestBegin,
                                           Trace::Event::IpcRequestEnd, 501};
        Trace::Mark(Trace::Event::GuestThreadReady, 501, 16);
        Trace::Mark(Trace::Event::GuestDispatch, 501, 0);
        constexpr u64 Work = 37;
        constexpr u64 Metadata = (1ULL << 32) | 3;
        Trace::Mark(Trace::Event::GpuQueuePushBegin, Work, Metadata);
        Trace::Mark(Trace::Event::GpuQueuePushEnd, Work, Metadata);
        {
            const Trace::ScopedGpuWork work{Work, Metadata};
            Check(Trace::CurrentGpuOrigin().work == Work, "GPU context was not installed");
            image.DownloadMemory(readback, std::span{&layout, 1});
            gate.scheduler.WaitSubmitted(gate.scheduler.Flush());
            gate.scheduler.Finish();
        }
        Check(Trace::CurrentGpuOrigin().work == 0, "GPU context leaked after its scope");
    }
    Trace::SetFrameChainMode(false);
    Check(Trace::FrameChainMode(), "capture mode changed while writers were active");
    for (u32 vsync = 0; vsync < 120; ++vsync) Trace::Mark(Trace::Event::Vsync);
    Check(Trace::GetCaptureStatus().state == Trace::CaptureState::Saved, "capture did not complete");
    Check(!std::memcmp(readback.mapped_span.data(), expected.texels.data(), layout.buffer_size),
          "diagnostic altered copied pixels");
    gate.runtime.FreeDeferredStagingBuffer(readback);
    Trace::Start(1);
    {
        const Trace::ScopedGpuWork old_capture{999, 1ULL << 32};
        Trace::Mark(Trace::Event::Vsync);
        Trace::Start(1);
        Check(Trace::CurrentGpuOrigin().work == 0, "old capture context leaked into a new capture");
        {
            const Trace::ScopedGpuWork nested{1000, 1ULL << 32};
            Check(Trace::CurrentGpuOrigin().work == 1000, "nested GPU context was not installed");
        }
        Check(Trace::CurrentGpuOrigin().work == 0, "stale parent context became current");
        Trace::Mark(Trace::Event::Vsync);
    }
    Trace::SetFrameChainMode(false);
    Check(!Trace::FrameChainMode(), "diagnostic could not be disabled after capture");
    gate.CheckDebugLayer("frame-chain native scheduler");
    Common::Log::Stop();
    std::cout << "PASS: bounded frame-chain context, arbitrary guests, real GPU bytes and recorder handoff; D3D12 debug clean\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
