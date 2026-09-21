#include "ui.h"

#include "log.h"
#include "updater.h"

#include <imgui.h>

#include <algorithm>

namespace raidcast {
namespace {

void Row(const char* label, const char* fmt, ...) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextUnformatted(label);
    ImGui::TableSetColumnIndex(1);
    va_list args;
    va_start(args, fmt);
    ImGui::TextV(fmt, args);
    va_end(args);
}

ImVec4 Health(bool ok) {
    return ok ? ImVec4(0.45f, 0.85f, 0.45f, 1.0f) : ImVec4(0.95f, 0.45f, 0.35f, 1.0f);
}

}  // namespace

bool HostPanel::Init(ID3D11Device* device, ID3D11DeviceContext* ctx, std::uint32_t w,
                     std::uint32_t h, std::string* error) {
    device_.copy_from(device);
    ctx_.copy_from(ctx);
    w_ = w;
    h_ = h;

    D3D11_TEXTURE2D_DESC d{};
    d.Width      = w;
    d.Height     = h;
    d.MipLevels  = 1;
    d.ArraySize  = 1;
    d.Format     = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc = {1, 0};
    d.Usage      = D3D11_USAGE_DEFAULT;
    d.BindFlags  = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device->CreateTexture2D(&d, nullptr, preview_.put()))) {
        if (error) *error = "preview texture allocation failed";
        return false;
    }
    if (FAILED(device->CreateShaderResourceView(preview_.get(), nullptr, preview_srv_.put()))) {
        if (error) *error = "preview SRV failed";
        return false;
    }
    return true;
}

void HostPanel::UpdatePreview(ID3D11Texture2D* bgra) {
    if (!preview_ || bgra == nullptr) return;
    std::lock_guard<std::mutex> lock(mu_);
    ctx_->CopyResource(preview_.get(), bgra);
}

void HostPanel::PushHistory(const HostStatus& s) {
    const double now = ImGui::GetTime();
    if (now - last_sample_ < 0.25) return;
    last_sample_ = now;

    mbps_history_[history_pos_] = static_cast<float>(s.mbps);
    fps_history_[history_pos_]  = static_cast<float>(s.fps);
    history_pos_ = (history_pos_ + 1) % mbps_history_.size();
}

HostPanel::Result HostPanel::Draw(const HostStatus& s, UpdateChannel* channel) {
    PushHistory(s);
    Result result;

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("RaidCast", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

    // --- status line --------------------------------------------------------
    if (s.capture_lost) {
        ImGui::TextColored(Health(false),
                           "CAPTURE LOST - the target window is gone. Nothing is being sent.");
    } else if (s.streaming && s.capture_quiet_s >= 2.0) {
        // The preview above is frozen on the same frame the viewer is stuck on.
        // Without this line the host has no way to tell that from a calm
        // moment in the game.
        ImGui::TextColored(ImVec4(0.9f, 0.8f, 0.4f, 1.0f),
                           "Not capturing (%.0fs) - the viewer's picture is frozen. "
                           "Click back into WoW.",
                           s.capture_quiet_s);
    } else if (s.streaming) {
        ImGui::TextColored(Health(true), "Streaming to %s", s.peer.c_str());
    } else {
        ImGui::TextColored(ImVec4(0.9f, 0.8f, 0.4f, 1.0f), "Waiting for viewer...");
    }
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 90.0f);
    if (ImGui::Button("Stop", ImVec2(80, 0))) result.stop = true;

    // Tailscale state sits above the preview because when it is wrong, nothing
    // below it matters.
    if (!s.tailnet_line.empty()) {
        ImGui::TextColored(Health(s.tailnet_ok), "Tailscale: %s", s.tailnet_line.c_str());
        if (!s.allow_summary.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("- %s", s.allow_summary.c_str());
        }
    }
    if (!s.tailnet_advice.empty())
        ImGui::TextColored(ImVec4(0.9f, 0.8f, 0.4f, 1.0f), "%s", s.tailnet_advice.c_str());

    ImGui::Separator();

    // --- preview ------------------------------------------------------------
    // Always visible. This is the host's proof of what is leaving the machine.
    ImGui::TextUnformatted("Preview - this is exactly what the viewer sees");
    {
        std::lock_guard<std::mutex> lock(mu_);
        const float avail = ImGui::GetContentRegionAvail().x;
        const float wide  = std::min(avail, 640.0f);
        const float tall  = w_ ? wide * static_cast<float>(h_) / static_cast<float>(w_) : 360.0f;
        ImGui::Image(reinterpret_cast<ImTextureID>(preview_srv_.get()), ImVec2(wide, tall));
    }

    ImGui::Separator();

    // --- stats --------------------------------------------------------------
    if (ImGui::BeginTable("stats", 2, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed, 150.0f);
        Row("Source", "%s  %ux%u", s.target.c_str(), s.width, s.height);
        Row("Encoder", "%s  (cap %u Mbps)", s.codec.c_str(), s.bitrate_cap_mbps);
        Row("Frame rate", "%.1f fps", s.fps);
        Row("Video bitrate", "%.2f Mbps", s.mbps);
        Row("Audio bitrate", "%.1f kbps", s.audio_kbps);
        Row("Encode latency", "%.2f ms p50 / %.2f ms p99", s.enc_p50_ms, s.enc_p99_ms);
        Row("Round trip", "%.2f ms", s.rtt_ms);
        Row("SRT buffer", "%d ms configured / %d ms queued", s.srt_latency_ms, s.sndbuf_ms);
        Row("Retransmits", "%lld", s.retrans);
        Row("Frames sent", "%llu", static_cast<unsigned long long>(s.frames));
        // Both are silent-by-default diagnostics: they only appear once the
        // fault they describe has actually happened.
        if (s.capture_restarts > 0) Row("Capture restarts", "%d", s.capture_restarts);
        if (s.keyframe_requests > 0) Row("Keyframes requested", "%d", s.keyframe_requests);
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::PlotLines("Mbps", mbps_history_.data(), static_cast<int>(mbps_history_.size()),
                     static_cast<int>(history_pos_), nullptr, 0.0f,
                     std::max(1.0f, *std::max_element(mbps_history_.begin(),
                                                      mbps_history_.end()) * 1.2f),
                     ImVec2(0, 60));
    ImGui::PlotLines("fps", fps_history_.data(), static_cast<int>(fps_history_.size()),
                     static_cast<int>(history_pos_), nullptr, 0.0f, 75.0f, ImVec2(0, 60));

    if (s.fps > 0 && s.fps < 45) {
        ImGui::TextColored(ImVec4(0.9f, 0.8f, 0.4f, 1.0f),
                           "Capture below 60 fps - WoW is probably not the foreground window.");
    }

    ImGui::Spacing();
    DrawLogPanel("Log", &log_open_, 180.0f);

    ImGui::Spacing();
    ImGui::Separator();
    if (DrawUpdateChannelCombo(channel)) result.channel_changed = true;

    ImGui::End();
    return result;
}

}  // namespace raidcast
