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
