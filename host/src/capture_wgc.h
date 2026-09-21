// Windows Graphics Capture bound to a single window.
//
// This is the whole of RaidCast's video input. It captures one HWND into a
// D3D11 texture with no DLL injected into the target process (.local/DESIGN.md D3).
//
// The privacy invariant lives here (DESIGN.md §7): a WindowCapture is bound to
// exactly one HWND for its lifetime. There is no re-target, no search-by-title
// and no fallback to display capture. If the target goes away the capture
// closes and stays closed.

#pragma once

#include <windows.h>
#include <d3d11.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace raidcast {

struct WindowInfo {
    HWND          hwnd = nullptr;
    std::uint32_t pid  = 0;
    std::uint32_t width  = 0;
    std::uint32_t height = 0;
    std::wstring  title;
    std::wstring  exe;  // file name only, e.g. L"Wow.exe"
};

// Top-level, visible, non-cloaked windows with a non-empty client area,
// optionally filtered to one executable name (case-insensitive).
std::vector<WindowInfo> EnumerateCaptureCandidates(const std::wstring& exe_filter = {});

struct CaptureFrame {
    ID3D11Texture2D* texture = nullptr;  // borrowed — valid only inside the callback
    std::uint32_t width  = 0;
    std::uint32_t height = 0;
    std::int64_t  content_time_100ns = 0;  // compositor timestamp, QPC-based, 100ns units
    std::int64_t  arrived_time_100ns = 0;  // same timebase, sampled on delivery
};

class WindowCapture {
public:
    // Invoked on a WGC worker thread, not the caller's.
    using FrameCallback = std::function<void(const CaptureFrame&)>;

    WindowCapture();
    ~WindowCapture();
    WindowCapture(const WindowCapture&)            = delete;
    WindowCapture& operator=(const WindowCapture&) = delete;

    bool Start(HWND target,
               ID3D11Device* device,
               FrameCallback on_frame,
               std::string* error = nullptr);
    void Stop();

    // Terminal once true: the target window was destroyed. Callers must surface
    // this rather than binding something else.
    bool closed() const;

    std::uint64_t frames() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace raidcast
