#include "reconnect.h"

#include <algorithm>

namespace raidcast {
namespace {

// Attempt immediately, then back off. Capped low: the host is a machine on the
// same tailnet that is probably restarting, not a congested public service, and
// a raid leader who alt-tabs back wants the picture now.
std::chrono::milliseconds BackoffFor(int attempt) {
    static constexpr int kSteps[] = {0, 1000, 2000, 3000, 5000};
    const int idx = std::min(attempt, static_cast<int>(std::size(kSteps)) - 1);
    return std::chrono::milliseconds(kSteps[idx]);
}

}  // namespace

Reconnector::~Reconnector() { Cancel(); }

void Reconnector::Start(const ReconnectTarget& target) {
    if (running_.load(std::memory_order_acquire)) return;

    cancel_.store(false, std::memory_order_release);
    ready_.store(false, std::memory_order_release);
    attempts_.store(0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(mu_);
        last_error_.clear();
        next_attempt_ = std::chrono::steady_clock::now();
    }

    if (thread_.joinable()) thread_.join();
    running_.store(true, std::memory_order_release);
    thread_ = std::thread(&Reconnector::Run, this, target);
}

void Reconnector::Cancel() {
    cancel_.store(true, std::memory_order_release);
    if (thread_.joinable()) thread_.join();
    running_.store(false, std::memory_order_release);

    // A link that connected just as the user cancelled would otherwise leak a
    // socket and leave the host believing it still has a viewer.
    std::lock_guard<std::mutex> lock(mu_);
    if (ready_.exchange(false, std::memory_order_acq_rel)) link_.Close();
}

SrtLink Reconnector::Take() {
    if (thread_.joinable()) thread_.join();
    running_.store(false, std::memory_order_release);
    ready_.store(false, std::memory_order_release);

    std::lock_guard<std::mutex> lock(mu_);
    return std::move(link_);
}

std::string Reconnector::last_error() const {
    std::lock_guard<std::mutex> lock(mu_);
    return last_error_;
}

double Reconnector::seconds_until_retry() const {
    std::lock_guard<std::mutex> lock(mu_);
    const auto now = std::chrono::steady_clock::now();
    if (next_attempt_ <= now) return 0.0;
    return std::chrono::duration<double>(next_attempt_ - now).count();
}

void Reconnector::Run(ReconnectTarget target) {
    for (int attempt = 0; !cancel_.load(std::memory_order_acquire); ++attempt) {
        // Wait out the backoff in short slices so Cancel() is responsive.
        const auto wait_until = std::chrono::steady_clock::now() + BackoffFor(attempt);
        {
            std::lock_guard<std::mutex> lock(mu_);
            next_attempt_ = wait_until;
        }
        while (std::chrono::steady_clock::now() < wait_until) {
            if (cancel_.load(std::memory_order_acquire)) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (cancel_.load(std::memory_order_acquire)) return;

        attempts_.fetch_add(1, std::memory_order_relaxed);

        SrtLink     candidate;
        std::string err;
        if (candidate.Connect(target.host, target.port, target.latency_ms, target.stream_id,
                              &err)) {
            std::lock_guard<std::mutex> lock(mu_);
            link_ = std::move(candidate);
            ready_.store(true, std::memory_order_release);
            return;
        }

        std::lock_guard<std::mutex> lock(mu_);
        last_error_ = err;
    }
}

}  // namespace raidcast
