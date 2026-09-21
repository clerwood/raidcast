#include "connect_ui.h"

#include "updater.h"

#include <imgui.h>

#include <cstdio>
#include <cstring>

namespace raidcast {
namespace {

ImVec4 Good() { return ImVec4(0.45f, 0.85f, 0.45f, 1.0f); }
ImVec4 Warn() { return ImVec4(0.90f, 0.80f, 0.40f, 1.0f); }
ImVec4 Bad()  { return ImVec4(0.95f, 0.45f, 0.35f, 1.0f); }

}  // namespace

void DrawWaitingPanel(const std::string& host, double waiting_s) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("RaidCast", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

    ImGui::TextColored(Good(), "Connected to %s", host.c_str());
    ImGui::Spacing();

    // Past a couple of seconds this is no longer "starting up", it is the host
    // not sending - and the host's own window will be saying so.
    if (waiting_s < 2.0) {
        ImGui::TextUnformatted("Waiting for the first frame...");
    } else {
        ImGui::TextColored(Warn(), "No video yet (%.0fs).", waiting_s);
        ImGui::TextDisabled(
            "The connection is up, so the host is not sending. Usually that means\n"
            "WoW is not running or not in Fullscreen (Windowed) on their machine.");
    }

    ImGui::End();
}

ReconnectChoice DrawReconnectPanel(const std::string& host, int attempts,
                                   double seconds_until_retry,
                                   const std::string& last_error, bool compact) {
    ReconnectChoice choice;

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    if (compact) {
        // Bottom centre, over the last frame. Deliberately not the middle of
        // the screen: whatever the viewer was reading when it froze — raid
        // frames, a boss timer — is still the most useful thing on it.
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f,
                                       vp->WorkPos.y + vp->WorkSize.y - 16),
                                ImGuiCond_Always, ImVec2(0.5f, 1.0f));
        ImGui::SetNextWindowBgAlpha(0.80f);
        ImGui::Begin("##reconnect", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoFocusOnAppearing);
    } else {
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("RaidCast", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
    }

    ImGui::TextColored(Warn(), "Lost the stream from %s", host.c_str());

    if (seconds_until_retry > 0.05)
        ImGui::Text("Reconnecting in %.0fs (attempt %d)...", seconds_until_retry, attempts + 1);
    else
        ImGui::Text("Reconnecting... (attempt %d)", attempts);

    // The host is usually mid-restart, and "connection setup failure" on loop
    // reads as broken rather than as working-as-intended.
    if (!last_error.empty()) ImGui::TextDisabled("Last attempt: %s", last_error.c_str());

    if (!compact) {
        ImGui::Spacing();
        ImGui::TextDisabled(
            "This keeps trying until it works. If they have quit for the night,\n"
            "pick another host or close the window.");
    }

    ImGui::Spacing();
    if (ImGui::Button("Pick another host", ImVec2(160, 0))) choice.pick_another = true;
    ImGui::SameLine();
    if (ImGui::Button("Quit", ImVec2(80, 0))) choice.quit = true;

    ImGui::End();
    return choice;
}

void ConnectPanel::Refresh() {
    status_  = QueryStatus();
    queried_ = true;

    // Preselect the only sensible choice when there is exactly one online peer,
    // which is the normal case for a two-person tailnet.
    selected_ = -1;
    int online_count = 0, last_online = -1;
    for (int i = 0; i < static_cast<int>(status_.peers.size()); ++i) {
        if (status_.peers[static_cast<std::size_t>(i)].online) {
            ++online_count;
            last_online = i;
        }
    }
    if (online_count == 1) selected_ = last_online;
}

ConnectChoice ConnectPanel::Draw(UpdateChannel* channel, const std::string& last_error) {
    if (!queried_) Refresh();

    ConnectChoice choice;

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("RaidCast", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

    ImGui::TextUnformatted("Connect to a host");
    ImGui::Separator();
    ImGui::Spacing();

    // --- Tailscale state ----------------------------------------------------
    if (status_.usable()) {
        ImGui::TextColored(Good(), "Tailscale: %s", status_.self_name.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", status_.self_ip.c_str());
    } else {
        ImGui::TextColored(Bad(), "Tailscale: %s",
                           status_.backend_state.empty() ? "unavailable"
                                                         : status_.backend_state.c_str());
    }
    if (const auto advice = AdviceFor(status_); !advice.empty())
        ImGui::TextColored(Warn(), "%s", advice.c_str());

    ImGui::Spacing();

    // --- peer list ----------------------------------------------------------
    const char* preview = "Select a host...";
    if (selected_ >= 0 && selected_ < static_cast<int>(status_.peers.size()))
        preview = status_.peers[static_cast<std::size_t>(selected_)].name.c_str();

    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 90.0f);
    if (ImGui::BeginCombo("##peers", preview)) {
        for (int i = 0; i < static_cast<int>(status_.peers.size()); ++i) {
            const auto& p = status_.peers[static_cast<std::size_t>(i)];
            ImGui::BeginDisabled(!p.online);
            if (ImGui::Selectable(p.name.c_str(), selected_ == i)) selected_ = i;
            ImGui::EndDisabled();

            ImGui::SameLine();
            if (!p.online)
                ImGui::TextDisabled("offline");
            else if (p.relayed)
                ImGui::TextColored(Warn(), "relayed via %s", p.relay.c_str());
            else
                ImGui::TextColored(Good(), "direct");
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh", ImVec2(80, 0))) Refresh();

    if (status_.usable() && status_.peers.empty()) {
        ImGui::TextColored(Warn(),
                           "No other machines on this tailnet yet. Invite the other person\n"
                           "from the Tailscale admin console, then press Refresh.");
    }

    // Selecting a relayed peer is allowed but should not be a surprise.
    if (selected_ >= 0 && selected_ < static_cast<int>(status_.peers.size())) {
        const auto& p = status_.peers[static_cast<std::size_t>(selected_)];
        if (p.relayed) {
            ImGui::TextColored(Warn(),
                               "This connection is relayed, not direct. Relays are shared and\n"
                               "TCP-based and will not carry video well. Ask the host to forward\n"
                               "UDP 41641.");
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextDisabled("...or enter an address or machine name directly");
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    const bool entered = ImGui::InputText("##manual", manual_, sizeof(manual_),
                                          ImGuiInputTextFlags_EnterReturnsTrue);
    if (entered || std::strlen(manual_) > 0) selected_ = -1;

    if (!last_error.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(Bad(), "%s", last_error.c_str());
    }

    ImGui::Spacing();

    const bool has_manual = std::strlen(manual_) > 0;
    const bool has_peer   = selected_ >= 0 &&
                            selected_ < static_cast<int>(status_.peers.size()) &&
                            status_.peers[static_cast<std::size_t>(selected_)].online;

    ImGui::BeginDisabled(!has_manual && !has_peer);
    if (ImGui::Button("Connect", ImVec2(120, 0)) || entered) {
        choice.connect = true;
        choice.host = has_manual ? std::string(manual_)
                                 : status_.peers[static_cast<std::size_t>(selected_)].ip;
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("Quit", ImVec2(80, 0))) choice.quit = true;

    ImGui::Spacing();
    ImGui::Separator();
    if (DrawUpdateChannelCombo(channel)) choice.channel_changed = true;

    ImGui::End();
    return choice;
}

}  // namespace raidcast
