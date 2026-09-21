// Update check and install, against GitHub Releases.
//
// Two binaries on two machines drift apart, and a version mismatch at 20:05 on a
// raid night is exactly the failure this project exists to design out
// (.local/DESIGN.md §12). The check is prompt-only and never silent: it offers,
// the user decides.
//
// The download is verified against the SHA-256 digest the GitHub API reports for
// the asset before anything is executed. RaidCast's binaries are unsigned, so
// that digest is the only integrity check available - but it is a real one, and
// it is checked over a separate TLS connection to api.github.com from the one
// that fetched the file.

#pragma once

#include "settings.h"

#include <atomic>
#include <memory>
#include <string>

namespace raidcast {

enum class UpdateState {
    None,         // nothing newer, or not checked yet
    Available,    // newer release found; waiting on the user
    Downloading,
    Verifying,
    Verified,     // downloaded and checksummed, launch suppressed (dry run)
    Launching,    // installer started; the app should exit
    Failed,
};

class Updater {
public:
    Updater();
    ~Updater();
    Updater(const Updater&)            = delete;
    Updater& operator=(const Updater&) = delete;

    // Fires off a background check. Never blocks, never throws, and failing to
    // reach GitHub is not an error the user needs to hear about.
    void CheckAsync(const std::string& current_version, UpdateChannel channel);

    // Draws the notification if one is pending. Call inside an ImGui frame.
    void DrawToast();

    UpdateState state() const;
    int         progress_percent() const;
    std::string latest_version() const;

    // True once the installer has been launched: the app must exit so the
    // installer can replace files that are currently in use.
    bool quit_requested() const;

    // Downloads and verifies the installer. `launch_installer = false` stops
    // after verification, which is how the update path is tested without
    // actually installing over the running build.
    void StartDownload(bool launch_installer = true);

    // Where the verified installer was written, once state() is Verified.
    std::string downloaded_path() const;

private:

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Combo box for the update channel. Returns true when the user changed it, so
// the caller can persist the choice.
bool DrawUpdateChannelCombo(UpdateChannel* channel);

// Exposed for testing: true when `latest` is newer than `current`.
// Both are dotted numeric versions, with or without a leading "v".
bool IsNewerVersion(const std::string& latest, const std::string& current);

}  // namespace raidcast
