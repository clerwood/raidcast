// Update check against GitHub Releases.
//
// Two binaries on two machines drift apart, and a version mismatch at 20:05 on
// a raid night is exactly the failure this project exists to design out
// (.local/DESIGN.md §12). The check is prompt-only and never silent: it offers,
// the user decides.

#pragma once

#include <atomic>
#include <memory>
#include <string>

namespace raidcast {

class Updater {
public:
    Updater();
    ~Updater();
    Updater(const Updater&)            = delete;
    Updater& operator=(const Updater&) = delete;

    // Fires off a background check. Never blocks, never throws, and failing to
    // reach GitHub is not an error the user needs to hear about.
    void CheckAsync(const std::string& current_version);

    // Draws the notification if one is pending. Call inside an ImGui frame.
    void DrawToast();

    bool        update_available() const;
    std::string latest_version() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Exposed for testing: true when `latest` is newer than `current`.
// Both are dotted numeric versions, with or without a leading "v".
bool IsNewerVersion(const std::string& latest, const std::string& current);

}  // namespace raidcast
