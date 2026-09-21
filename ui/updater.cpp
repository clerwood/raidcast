#include "updater.h"

#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <winhttp.h>

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <fstream>
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
    std::size_t      i   = (!v.empty() && (v[0] == 'v' || v[0] == 'V')) ? 1 : 0;
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

std::string Narrow(const std::wstring& w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

HINTERNET OpenSession() {
    return WinHttpOpen(L"RaidCast", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                       WINHTTP_NO_PROXY_BYPASS, 0);
}

std::string HttpsGet(const wchar_t* host, const wchar_t* path) {
    std::string out;
    HINTERNET   session = OpenSession();
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
                    if (out.size() > 4u << 20) break;  // release payloads are small
                }
            }
            WinHttpCloseHandle(request);
        }
        WinHttpCloseHandle(connect);
    }
    WinHttpCloseHandle(session);
    return out;
}

// Downloads `url` to `path`, reporting 0-100 through `progress`. WinHTTP follows
// the cross-host redirect from github.com to the asset CDN by itself.
bool DownloadToFile(const std::wstring& url, const std::wstring& path,
                    std::atomic<int>* progress, std::string* error) {
    URL_COMPONENTS parts{};
    parts.dwStructSize   = sizeof(parts);
    wchar_t host[256]    = {};
    wchar_t rest[4096]   = {};
    parts.lpszHostName   = host;
    parts.dwHostNameLength = 256;
    parts.lpszUrlPath    = rest;
    parts.dwUrlPathLength = 4096;

    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) {
        if (error) *error = "malformed download URL";
        return false;
    }
    if (parts.nScheme != INTERNET_SCHEME_HTTPS) {
        if (error) *error = "refusing a non-HTTPS download";
        return false;
    }

    HINTERNET session = OpenSession();
    if (!session) {
        if (error) *error = "could not start an HTTP session";
        return false;
    }

    bool ok = false;
    if (HINTERNET connect = WinHttpConnect(session, host, parts.nPort, 0)) {
        if (HINTERNET request =
                WinHttpOpenRequest(connect, L"GET", rest, nullptr, WINHTTP_NO_REFERER,
                                   WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)) {
            if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                   WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(request, nullptr)) {
                DWORD status = 0, len = sizeof(status);
                WinHttpQueryHeaders(request,
                                    WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX, &status, &len,
                                    WINHTTP_NO_HEADER_INDEX);
                if (status == 200) {
                    DWORDLONG total = 0;
                    DWORD     tlen  = sizeof(total);
                    WinHttpQueryHeaders(
                        request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER64,
                        WINHTTP_HEADER_NAME_BY_INDEX, &total, &tlen, WINHTTP_NO_HEADER_INDEX);

                    std::ofstream out(path, std::ios::binary | std::ios::trunc);
                    if (out) {
                        std::vector<char> buf(64 * 1024);
                        DWORDLONG         got = 0;
                        for (;;) {
                            DWORD avail = 0;
                            if (!WinHttpQueryDataAvailable(request, &avail) || avail == 0) break;
                            if (avail > buf.size()) buf.resize(avail);
                            DWORD read = 0;
                            if (!WinHttpReadData(request, buf.data(), avail, &read) || read == 0)
                                break;
                            out.write(buf.data(), read);
                            got += read;
                            if (total > 0 && progress)
                                progress->store(static_cast<int>(got * 100 / total),
                                                std::memory_order_relaxed);
                        }
                        out.close();
                        ok = out.good() && (total == 0 || got == total);
                        if (!ok && error) *error = "the download ended early";
                    } else if (error) {
                        *error = "could not write to " + Narrow(path);
                    }
                } else if (error) {
                    *error = "the download returned HTTP " + std::to_string(status);
                }
            } else if (error) {
                *error = "the download request failed";
            }
            WinHttpCloseHandle(request);
        }
        WinHttpCloseHandle(connect);
    }
    WinHttpCloseHandle(session);
    return ok;
}

// Lowercase hex SHA-256 of a file, or empty on failure.
std::string Sha256File(const std::wstring& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};

    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return {};

    BCRYPT_HASH_HANDLE hash = nullptr;
    std::string        result;
    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        std::vector<char> buf(64 * 1024);
        bool              failed = false;
        while (in.read(buf.data(), static_cast<std::streamsize>(buf.size())) || in.gcount() > 0) {
            if (BCryptHashData(hash, reinterpret_cast<PUCHAR>(buf.data()),
                               static_cast<ULONG>(in.gcount()), 0) != 0) {
                failed = true;
                break;
            }
        }
        unsigned char digest[32] = {};
        if (!failed && BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0) {
            static const char* kHex = "0123456789abcdef";
            for (unsigned char b : digest) {
                result.push_back(kHex[b >> 4]);
                result.push_back(kHex[b & 0x0F]);
            }
        }
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return result;
}

std::wstring TempPathFor(const std::string& filename) {
    wchar_t dir[MAX_PATH] = {};
    if (GetTempPathW(MAX_PATH, dir) == 0) return {};
    return std::wstring(dir) + Widen(filename);
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
    std::thread        check_worker;
    std::thread        download_worker;
    mutable std::mutex mu;

    std::string latest;
    std::string page_url;
    std::string asset_url;
    std::string asset_name;
    std::string asset_digest;  // lowercase hex sha256
    std::string failure;
    std::string downloaded;

    std::atomic<UpdateState> state{UpdateState::None};
    std::atomic<int>         progress{0};
    std::atomic<bool>        quit{false};
    bool                     dismissed = false;
};

Updater::Updater() : impl_(std::make_unique<Impl>()) {}
Updater::~Updater() {
    if (!impl_) return;
    if (impl_->check_worker.joinable()) impl_->check_worker.join();
    if (impl_->download_worker.joinable()) impl_->download_worker.join();
}

UpdateState Updater::state() const { return impl_->state.load(std::memory_order_acquire); }
int  Updater::progress_percent() const { return impl_->progress.load(std::memory_order_relaxed); }
bool Updater::quit_requested() const { return impl_->quit.load(std::memory_order_acquire); }

std::string Updater::latest_version() const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->latest;
}

void Updater::CheckAsync(const std::string& current_version, UpdateChannel channel) {
    if (impl_->check_worker.joinable()) return;

    impl_->check_worker = std::thread([this, current_version, channel] {
        // Deliberately NOT /releases/latest: that endpoint excludes pre-releases,
        // so while every 0.x tag ships as a pre-release it returns 404 and no
        // update is ever offered. List releases and choose ourselves.
        const std::wstring path =
            L"/repos/" + Widen(RAIDCAST_REPO) + L"/releases?per_page=20";
        const std::string body = HttpsGet(L"api.github.com", path.c_str());
        if (body.empty()) return;

        const bool accept_prerelease = channel == UpdateChannel::Beta;

        std::string best_tag, best_url, best_asset, best_asset_name, best_digest;
        try {
            const auto releases = nlohmann::json::parse(body);
            if (!releases.is_array()) return;

            for (const auto& rel : releases) {
                if (!rel.is_object()) continue;
                if (rel.value("draft", false)) continue;
                if (rel.value("prerelease", false) && !accept_prerelease) continue;

                const auto tag = rel.value("tag_name", std::string{});
                if (tag.empty() || !IsNewerVersion(tag, current_version)) continue;
                // Do not trust the array order; keep the highest version seen.
                if (!best_tag.empty() && !IsNewerVersion(tag, best_tag)) continue;

                std::string asset_url, asset_name, digest;
                if (auto assets = rel.find("assets"); assets != rel.end() && assets->is_array()) {
                    for (const auto& a : *assets) {
                        const auto name = a.value("name", std::string{});
                        if (name.size() < 9 || name.rfind("Setup.exe") != name.size() - 9)
                            continue;
                        asset_url  = a.value("browser_download_url", std::string{});
                        asset_name = name;
                        // "sha256:<hex>"; anything else we treat as absent.
                        const auto d = a.value("digest", std::string{});
                        if (d.rfind("sha256:", 0) == 0) digest = d.substr(7);
                        break;
                    }
                }

                best_tag        = tag;
                best_url        = rel.value("html_url", std::string{});
                best_asset      = asset_url;
                best_asset_name = asset_name;
                best_digest     = digest;
            }
        } catch (const std::exception&) {
            return;  // a malformed reply is not worth telling the user about
        }
        if (best_tag.empty()) return;

        std::lock_guard<std::mutex> lock(impl_->mu);
        impl_->latest       = best_tag;
        impl_->page_url     = best_url;
        impl_->asset_url    = best_asset;
        impl_->asset_name   = best_asset_name;
        impl_->asset_digest = best_digest;
        impl_->state.store(UpdateState::Available, std::memory_order_release);
    });
}

std::string Updater::downloaded_path() const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->downloaded;
}

void Updater::StartDownload(bool launch_installer) {
    if (impl_->download_worker.joinable()) return;

    std::string url, name, digest;
    {
        std::lock_guard<std::mutex> lock(impl_->mu);
        url    = impl_->asset_url;
        name   = impl_->asset_name;
        digest = impl_->asset_digest;
    }
    if (url.empty()) {
        std::lock_guard<std::mutex> lock(impl_->mu);
        impl_->failure = "That release has no installer attached.";
        impl_->state.store(UpdateState::Failed, std::memory_order_release);
        return;
    }

    impl_->progress.store(0, std::memory_order_relaxed);
    impl_->state.store(UpdateState::Downloading, std::memory_order_release);

    impl_->download_worker = std::thread([this, url, name, digest, launch_installer] {
        const std::wstring path = TempPathFor(name);
        auto fail = [&](const std::string& why) {
            std::lock_guard<std::mutex> lock(impl_->mu);
            impl_->failure = why;
            impl_->state.store(UpdateState::Failed, std::memory_order_release);
        };
        if (path.empty()) return fail("Could not find a temporary directory.");

        std::string err;
        if (!DownloadToFile(Widen(url), path, &impl_->progress, &err)) return fail(err);

        impl_->state.store(UpdateState::Verifying, std::memory_order_release);

        // The binaries are unsigned, so this digest is the only integrity check
        // available. Skipping it when GitHub does not report one would make the
        // check decorative, so treat its absence as a failure.
        if (digest.empty()) {
            DeleteFileW(path.c_str());
            return fail("GitHub did not publish a checksum for that installer.");
        }
        const std::string actual = Sha256File(path);
        if (actual.empty()) {
            DeleteFileW(path.c_str());
            return fail("Could not checksum the download.");
        }
        if (actual != digest) {
            DeleteFileW(path.c_str());
            return fail("The download did not match its published checksum and was deleted.");
        }

        {
            std::lock_guard<std::mutex> lock(impl_->mu);
            impl_->downloaded = Narrow(path);
        }
        if (!launch_installer) {
            impl_->state.store(UpdateState::Verified, std::memory_order_release);
            return;
        }

        impl_->state.store(UpdateState::Launching, std::memory_order_release);

        // No silent flags: the user should see what is installing. RaidCast must
        // then exit, because the installer cannot replace an executable that is
        // still running.
        const auto rc = ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr,
                                      SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(rc) <= 32) return fail("Could not start the installer.");

        impl_->quit.store(true, std::memory_order_release);
    });
}

void Updater::DrawToast() {
    const UpdateState st = state();
    if (st == UpdateState::None || impl_->dismissed) return;

    std::string latest, page_url, failure;
    {
        std::lock_guard<std::mutex> lock(impl_->mu);
        latest   = impl_->latest;
        page_url = impl_->page_url;
        failure  = impl_->failure;
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

    switch (st) {
        case UpdateState::Available:
            ImGui::TextColored(ImVec4(0.55f, 0.8f, 1.0f, 1.0f), "RaidCast %s is available",
                               latest.c_str());
            ImGui::TextDisabled("You are running %s", RAIDCAST_VERSION);
            ImGui::Spacing();
            if (ImGui::Button("Update now")) StartDownload();
            ImGui::SameLine();
            if (ImGui::Button("Release notes") && !page_url.empty())
                ShellExecuteA(nullptr, "open", page_url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            ImGui::SameLine();
            if (ImGui::Button("Later")) impl_->dismissed = true;
            break;

        case UpdateState::Downloading:
            ImGui::Text("Downloading RaidCast %s...", latest.c_str());
            ImGui::ProgressBar(progress_percent() / 100.0f, ImVec2(260, 0));
            break;

        case UpdateState::Verifying:
            ImGui::Text("Checking the download...");
            break;

        case UpdateState::Verified:
            ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1.0f),
                               "Downloaded and verified (not installed).");
            ImGui::SameLine();
            if (ImGui::Button("Dismiss")) impl_->dismissed = true;
            break;

        case UpdateState::Launching:
            ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1.0f),
                               "Installer started - RaidCast will close.");
            break;

        case UpdateState::Failed:
            ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.35f, 1.0f), "Update failed");
            ImGui::TextWrapped("%s", failure.c_str());
            ImGui::Spacing();
            if (ImGui::Button("Open release page") && !page_url.empty())
                ShellExecuteA(nullptr, "open", page_url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            ImGui::SameLine();
            if (ImGui::Button("Dismiss")) impl_->dismissed = true;
            break;

        case UpdateState::None:
            break;
    }

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
