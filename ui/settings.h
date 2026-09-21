// Persistent user settings.
//
// Small on purpose: this exists because a preference you have to pass on the
// command line every time is not a preference. Stored as JSON under
// %APPDATA%\RaidCast so it survives reinstalls and upgrades.

#pragma once

#include <string>

namespace raidcast {

enum class UpdateChannel {
    Stable,  // only full releases
    Beta,    // pre-releases too
};

const char* ToString(UpdateChannel c);
UpdateChannel ChannelFromString(const std::string& s, UpdateChannel fallback);

struct Settings {
    UpdateChannel update_channel = UpdateChannel::Stable;
};

// Reads the settings file. When none exists, the channel defaults to Beta for a
// build that is itself a pre-release (a 0.x or suffixed version), and Stable
// otherwise — so a beta tester is offered betas without having to ask, and a
// stable user is not.
Settings LoadSettings(const std::string& own_version);

bool SaveSettings(const Settings& s, std::string* error = nullptr);

// Absolute path of the settings file, for diagnostics.
std::string SettingsPath();

}  // namespace raidcast
