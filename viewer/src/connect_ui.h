// The viewer's pre-connect screen.
//
// Nobody should have to read an IP address out over voice chat, so the host is
// picked from a list of tailnet peers (.local/DESIGN.md §10). Each entry says
// whether the path is direct or relayed, because that single fact decides whether
// the session will work at all - and the moment to know is before connecting,
// not after the picture is already bad.

#pragma once

#include "settings.h"
#include "tailscale.h"

#include <string>

namespace raidcast {

struct ConnectChoice {
    bool        connect         = false;
    bool        quit            = false;
    bool        channel_changed = false;  // caller should persist the new value
    std::string host;
};

struct ReconnectChoice {
    bool quit         = false;
    bool pick_another = false;  // give up on this host, go back to the picker
};

// The "we lost the stream" banner, shown while a Reconnector retries in the
// background. Drawn over the last video frame when there is one (`compact`),
// which keeps the frozen picture visible and says why it is frozen, or as a
// full panel when the stream never got as far as a frame.
ReconnectChoice DrawReconnectPanel(const std::string& host, int attempts,
                                   double seconds_until_retry,
                                   const std::string& last_error, bool compact);

// Shown between connecting and the first decoded frame. A connected viewer
// with nothing to show is otherwise indistinguishable from a hung one.
void DrawWaitingPanel(const std::string& host, double waiting_s);

class ConnectPanel {
public:
    // Queried once on first draw and then only when the user asks. Peer
    // membership changes on the order of days; a timer here would exist purely
    // to spawn a subprocess every few seconds.
    void Refresh();

    ConnectChoice Draw(UpdateChannel* channel, const std::string& last_error = {});

private:
    TailStatus status_;
    bool       queried_  = false;
    bool       refreshing_ = false;
    int        selected_ = -1;
    char       manual_[128] = {};
};

}  // namespace raidcast
