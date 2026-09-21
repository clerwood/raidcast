#include "overlay.h"

#include "log.h"

#include <imgui.h>

#include <string>

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

}  // namespace

void DrawViewerOverlay(const ViewerStats& s, bool* visible, ViewerControls* controls) {
    if (ImGui::IsKeyPressed(ImGuiKey_Tab, false)) *visible = !*visible;
    // Volume is reachable without opening the stats panel: it is a control, not a
    // diagnostic, and wanting it quieter mid-pull is not a debugging session.
    const ImGuiViewport* vol_vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vol_vp->WorkPos.x + vol_vp->WorkSize.x - 8,
                                   vol_vp->WorkPos.y + 8),
                            ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::Begin("##volume", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);
    if (ImGui::Button(controls->muted ? "Unmute" : "Mute", ImVec2(64, 0))) {
        controls->muted  = !controls->muted;
        controls->changed = true;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(controls->muted);
    ImGui::SetNextItemWidth(130.0f);
    if (ImGui::SliderFloat("##vol", &controls->volume, 0.0f, 2.0f, "%.2fx",
                           ImGuiSliderFlags_AlwaysClamp)) {
        controls->changed = true;
    }
    ImGui::EndDisabled();
    ImGui::End();

    if (!*visible) {
        // Always leave a hint: a viewer who cannot find the diagnostics has no
        // diagnostics.
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + 8, vp->WorkPos.y + 8));
        ImGui::SetNextWindowBgAlpha(0.35f);
        ImGui::Begin("##hint", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings);
        ImGui::TextDisabled("Tab: stats");
        ImGui::End();
        return;
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + 8, vp->WorkPos.y + 8));
    ImGui::SetNextWindowBgAlpha(0.80f);
    ImGui::Begin("RaidCast stats", nullptr,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);

    if (ImGui::BeginTable("vstats", 2, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed, 140.0f);
        Row("Host", "%s", s.host.c_str());
        Row("Video", "%ux%u @ %.1f fps", s.width, s.height, s.fps);
        Row("Bitrate", "%.2f Mbps", s.mbps);
        Row("Decode", "%.2f ms p50", s.decode_p50);
        Row("Round trip", "%.2f ms", s.rtt_ms);
        Row("SRT buffer", "%d ms", s.srt_latency_ms);
        Row("Packets lost", "%lld", s.pkt_lost);
        Row("Retransmits", "%lld", s.pkt_retrans);
        Row("Frames dropped", "%llu", static_cast<unsigned long long>(s.reasm_dropped));
        Row("Bad packets", "%llu", static_cast<unsigned long long>(s.packets_bad));
        if (s.audio_ok) {
            Row("Audio queue", "%u ms", s.audio_queue_ms);
            const auto level = [](float db) {
                return db <= -119.0f ? std::string("silent")
                                     : std::to_string(static_cast<int>(db)) + " dBFS";
            };
            Row("Audio in", "%s", level(s.audio_peak_db).c_str());
            Row("Audio out", "%s", level(s.audio_out_db).c_str());
            Row("Audio underruns", "%llu", static_cast<unsigned long long>(s.audio_underruns));
        } else {
            Row("Audio", "unavailable");
        }
        ImGui::EndTable();
    }

    // The plain-English line: worth more than any individual number at 20:05.
    ImGui::Separator();
    if (s.fps <= 0)
        ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.35f, 1.0f), "No video arriving.");
    else if (s.dropped_recent > 0)
        ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.35f, 1.0f),
                           "Dropping frames - the link cannot keep up.");
    else if (s.lost_recent > 0)
        ImGui::TextColored(ImVec4(0.9f, 0.8f, 0.4f, 1.0f),
                           "Recovering lost packets - quality may dip.");
    else
        ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1.0f), "Healthy.");

    ImGui::Spacing();
    static bool log_open = false;
    DrawLogPanel("Log", &log_open, 140.0f);

    ImGui::TextDisabled("Tab: hide");
    ImGui::End();
}

}  // namespace raidcast
