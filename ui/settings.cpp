#include "settings.h"

#include <windows.h>
#include <shlobj.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <fstream>

namespace raidcast {
namespace {

std::string AppDataDir() {
    if (const char* appdata = std::getenv("APPDATA"); appdata && *appdata)
        return std::string(appdata) + "\\RaidCast";
    return ".";
}

bool IsPrereleaseVersion(const std::string& v) {
    return v.rfind("0.", 0) == 0 || v.find('-') != std::string::npos;
}

}  // namespace

const char* ToString(UpdateChannel c) {
    return c == UpdateChannel::Beta ? "beta" : "stable";
}

UpdateChannel ChannelFromString(const std::string& s, UpdateChannel fallback) {
    if (s == "beta") return UpdateChannel::Beta;
    if (s == "stable") return UpdateChannel::Stable;
    return fallback;
}

std::string SettingsPath() { return AppDataDir() + "\\settings.json"; }

Settings LoadSettings(const std::string& own_version) {
    Settings s;
    s.update_channel =
        IsPrereleaseVersion(own_version) ? UpdateChannel::Beta : UpdateChannel::Stable;

    std::ifstream in(SettingsPath());
    if (!in) return s;  // first run: the inferred default stands

    try {
        nlohmann::json j;
        in >> j;
        if (auto it = j.find("update_channel"); it != j.end() && it->is_string())
            s.update_channel = ChannelFromString(it->get<std::string>(), s.update_channel);
        if (auto it = j.find("volume"); it != j.end() && it->is_number())
            s.volume = std::clamp(it->get<float>(), 0.0f, 2.0f);
        if (auto it = j.find("muted"); it != j.end() && it->is_boolean())
            s.muted = it->get<bool>();
    } catch (const std::exception&) {
        // A corrupt settings file must not stop the app starting.
    }
    return s;
}

bool SaveSettings(const Settings& s, std::string* error) {
    const std::string dir = AppDataDir();
    if (!CreateDirectoryA(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        if (error) *error = "cannot create " + dir;
        return false;
    }

    std::ofstream out(SettingsPath(), std::ios::trunc);
    if (!out) {
        if (error) *error = "cannot write " + SettingsPath();
        return false;
    }
    const nlohmann::json j{{"update_channel", ToString(s.update_channel)},
                           {"volume", s.volume},
                           {"muted", s.muted}};
    out << j.dump(2) << '\n';
    return out.good();
}

}  // namespace raidcast
