#pragma once

#include <windows.h>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

struct Bitmap {
    unsigned width{};
    unsigned height{};
    std::vector<std::uint32_t> pixels;
    explicit operator bool() const { return !pixels.empty(); }
};

inline constexpr UINT CaptureReadyMessage = WM_APP + 1;

enum class CaptureIssue { None, Timeout, Unsupported, Protected, Hdr };

struct CaptureResult {
    unsigned request{};
    CaptureIssue issue{CaptureIssue::Unsupported};
    Bitmap bitmap;
};

class DesktopCapture {
public:
    explicit DesktopCapture(HWND receiver);
    ~DesktopCapture();
    unsigned Start(HMONITOR monitor, ULONGLONG deadline);
    void Cancel();
    std::optional<CaptureResult> TakeResult();

private:
    struct Job { HMONITOR monitor{}; ULONGLONG deadline{}; unsigned id{}; };
    void Run(std::stop_token stop);
    CaptureResult Capture(const Job& job);

    HWND receiver_;
    std::atomic<unsigned> generation_{};
    std::mutex mutex_;
    std::condition_variable_any changed_;
    std::optional<Job> pending_;
    std::optional<CaptureResult> complete_;
    std::jthread worker_;
};
