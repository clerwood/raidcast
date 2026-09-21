#include "updater.h"

#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

#ifndef RAIDCAST_REPO
#define RAIDCAST_REPO "OWNER/raidcast"
#endif

namespace raidcast {
namespace {

std::vector<int> ParseVersion(const std::string& v) {
    std::vector<int> parts;
    std::size_t      i = (!v.empty() && (v[0] == 'v' || v[0] == 'V')) ? 1 : 0;
    int              cur = 0;
    bool             any = false;
    for (; i <= v.size(); ++i) {
        if (i < v.size() && v[i] >= '0' && v[i] <= '9') {
            cur = cur * 10 + (v[i] - '0');
            any = true;
        } else {
            if (any) parts.push_back(cur);
            cur = 0;
            any = false;
            // Stop at a pre-release suffix such as "-beta".
            if (i < v.size() && v[i] != '.') break;
        }
    }
    return parts;
}

std::wstring Widen(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

std::string HttpsGet(const wchar_t* host, const wchar_t* path) {
    std::string out;
    HINTERNET session = WinHttpOpen(L"RaidCast", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return out;

    if (HINTERNET connect = WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0)) {
        if (HINTERNET request =
                WinHttpOpenRequest(connect, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                   WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)) {
            if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                   WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(request, nullptr)) {
                DWORD avail = 0;
                while (WinHttpQueryDataAvailable(request, &avail) && avail > 0) {
                    std::string chunk(avail, '\0');
                    DWORD       read = 0;
                    if (!WinHttpReadData(request, chunk.data(), avail, &read)) break;
                    out.append(chunk, 0, read);
                    if (out.size() > 1u << 20) break;  // releases payloads are small
                }
            }
            WinHttpCloseHandle(request);
        }
        WinHttpCloseHandle(connect);
    }
    WinHttpCloseHandle(session);
    return out;
}

}  // namespace

bool IsNewerVersion(const std::string& latest, const std::string& current) {
    const auto a = ParseVersion(latest);
    const auto b = ParseVersion(current);
    if (a.empty()) return false;
    for (std::size_t i = 0; i < a.size() || i < b.size(); ++i) {
        const int x = i < a.size() ? a[i] : 0;
        const int y = i < b.size() ? b[i] : 0;
        if (x != y) return x > y;
    }
    return false;
}

struct Updater::Impl {
    std::thread       worker;
    mutable std::mutex mu;
    std::string       latest;
    std::string       page_url;
    std::atomic<bool> available{false};
    bool              dismissed = false;
};

Updater::Updater() : impl_(std::make_unique<Impl>()) {}
Updater::~Updater() {
    if (impl_ && impl_->worker.joinable()) impl_->worker.join();
}

bool Updater::update_available() const {
    return impl_ && impl_->available.load(std::memory_order_acquire) && !impl_->dismissed;
}

std::string Updater::latest_version() const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->latest;
}

void Updater::CheckAsync(const std::string& current_version, UpdateChannel channel) {
    if (impl_->worker.joinable()) return;

    impl_->worker = std::thread([this, current_version, channel] {
        // Deliberately NOT /releases/latest: that endpoint excludes pre-releases,
        // so while every 0.x tag ships as a pre-release it returns 404 and no
        // update is ever offered. List releases and choose ourselves.
        //
        // RAIDCAST_REPO is a narrow macro, so the path is built at runtime rather
        // than by literal concatenation.
        const std::wstring path =
            L"/repos/" + Widen(RAIDCAST_REPO) + L"/releases?per_page=20";
        const std::string body = HttpsGet(L"api.github.com", path.c_str());
        if (const char* dbg = std::getenv("RAIDCAST_UPDATE_DEBUG"); dbg && *dbg) {
            std::fprintf(stderr, "[update] %zu bytes: %.200s\n", body.size(), body.c_str());
        }
        if (body.empty()) return;

        const bool accept_prerelease = channel == UpdateChannel::Beta;

        std::string best_tag, best_url;
        try {
            const auto releases = nlohmann::json::parse(body);
            if (!releases.is_array()) return;

            for (const auto& rel : releases) {
                if (!rel.is_object()) continue;
                if (rel.value("draft", false)) continue;
                if (rel.value("prerelease", false) && !accept_prerelease) continue;

                const auto tag = rel.value("tag_name", std::string{});
                if (tag.empty()) continue;
                if (!IsNewerVersion(tag, current_version)) continue;
                // Do not trust the array order; keep the highest version seen.
                if (!best_tag.empty() && !IsNewerVersion(tag, best_tag)) continue;

                best_tag = tag;
                best_url = rel.value("html_url", std::string{});
            }
        } catch (const std::exception&) {
            return;  // a malformed reply is not worth telling the user about
        }
        if (best_tag.empty()) return;

        std::lock_guard<std::mutex> lock(impl_->mu);
        impl_->latest   = best_tag;
        impl_->page_url = best_url;
        impl_->available.store(true, std::memory_order_release);
    });
}

void Updater::DrawToast() {
    if (!update_available()) return;

    std::string latest, url;
    {
        std::lock_guard<std::mutex> lock(impl_->mu);
        latest = impl_->latest;
        url    = impl_->page_url;
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - 20.0f,
                                   vp->WorkPos.y + vp->WorkSize.y - 20.0f),
                            ImGuiCond_Always, ImVec2(1.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.95f);
    ImGui::Begin("##update", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings);

    ImGui::TextColored(ImVec4(0.55f, 0.8f, 1.0f, 1.0f), "RaidCast %s is available",
                       latest.c_str());
    ImGui::TextDisabled("You are running %s", RAIDCAST_VERSION);
    ImGui::Spacing();

    if (ImGui::Button("Download") && !url.empty()) {
        // Opens the release page rather than installing silently: the host is
        // mid-session as often as not, and an unattended restart is worse than
        // an out-of-date build.
        ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        impl_->dismissed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Later")) impl_->dismissed = true;

    ImGui::End();
}

bool DrawUpdateChannelCombo(UpdateChannel* channel) {
    if (channel == nullptr) return false;

    static const char* kLabels[] = {"Stable only", "Include betas"};
    int current = *channel == UpdateChannel::Beta ? 1 : 0;

    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::Combo("Updates", &current, kLabels, 2)) {
        const auto chosen = current == 1 ? UpdateChannel::Beta : UpdateChannel::Stable;
        if (chosen != *channel) {
            *channel = chosen;
            return true;
        }
    }
    return false;
}

}  // namespace raidcast
