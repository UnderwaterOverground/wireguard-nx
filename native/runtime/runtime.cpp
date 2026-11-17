#include "runtime.hpp"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <vector>

#include "wgnx_app.h"
#include "wgnx_update.h"
#include "vsock.hpp"

namespace wgnx {

thread_local int Runtime::internal_depth_ = 0;

namespace {

#ifndef WGNX_NO_STDIO
std::vector<uint8_t> ReadFile(const std::string &path) {
    std::vector<uint8_t> out;
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return out;
    uint8_t buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.insert(out.end(), buf, buf + n);
    std::fclose(f);
    return out;
}
#endif

// A loopback UDP pair used to interrupt the engine thread's poll().
bool MakeWakePair(int *rx, int *tx) {
    *rx = socket(AF_INET, SOCK_DGRAM, 0);
    *tx = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof a;
    if (*rx < 0 || *tx < 0 || bind(*rx, reinterpret_cast<sockaddr *>(&a), sizeof a) != 0 ||
        getsockname(*rx, reinterpret_cast<sockaddr *>(&a), &len) != 0 ||
        connect(*tx, reinterpret_cast<sockaddr *>(&a), sizeof a) != 0) {
        if (*rx >= 0) close(*rx);
        if (*tx >= 0) close(*tx);
        *rx = *tx = -1;
        return false;
    }
    fcntl(*rx, F_SETFL, fcntl(*rx, F_GETFL, 0) | O_NONBLOCK);
    fcntl(*tx, F_SETFL, fcntl(*tx, F_GETFL, 0) | O_NONBLOCK);
    return true;
}

// A socket the system has taken away (Horizon drops them all when the
// network goes away without a sleep), as opposed to one that is just busy.
bool SocketGone(int e) { return e == EPIPE || e == EBADF || e == ENOTSOCK || e == ENOTCONN || e == ECONNREFUSED; }

// How often the loop may try to replace a dropped socket.
constexpr uint64_t kRepairEveryNs = 1'000'000'000;

}  // namespace

#ifndef WGNX_NO_STDIO  // reads the conf with stdio; embedders without it use Adopt()
std::unique_ptr<Runtime> Runtime::Start(RuntimeConfig cfg, std::string *error) {
    InternalScope internal;
    std::unique_ptr<Runtime> rt(new Runtime());
    rt->cfg_ = std::move(cfg);
    const RuntimeConfig &c = rt->cfg_;

    uint8_t seed[32];
    wgnx_platform_random(seed, sizeof seed);
    wgnx_seed_rng(seed);
    std::vector<uint8_t> conf = ReadFile(c.conf_path);
    if (conf.empty()) {
        *error = "cannot read " + c.conf_path;
        return nullptr;
    }
    bool corrected = false;
    uint64_t unix_time = wgnx_sane_unix_time(static_cast<uint64_t>(std::time(nullptr)), WGNX_UPDATE_HOST, &corrected);
    if (unix_time == 0) unix_time = static_cast<uint64_t>(std::time(nullptr));
    rt->engine_ = wgnx_engine_new(wgnx_driver_now_ns(), unix_time);
    WgnxConfig wc{};
    wc.conf = reinterpret_cast<const char *>(conf.data());
    wc.conf_len = conf.size();
    char err[160];
    if (!wgnx_engine_reload(rt->engine_, &wc, wgnx_driver_now_ns(), err, sizeof err)) {
        *error = c.conf_path + ": " + err;
        return nullptr;
    }
    WgnxPrefix routes[VSock::MaxRoutes];
    const size_t n = wgnx_engine_routes(rt->engine_, routes, VSock::MaxRoutes);
    VSock::SetRoutes(routes, std::min(n, VSock::MaxRoutes));
    Runtime *self = rt.get();
    rt->driver_ = wgnx_driver_new(
        rt->engine_,
        [](void *ctx, const WgnxEvent *ev) {
            auto *r = static_cast<Runtime *>(ctx);
            if (r->cfg_.on_event) r->cfg_.on_event(*ev);
        },
        self);
    const uint16_t port = c.udp_port ? c.udp_port : wgnx_engine_listen_port(rt->engine_);
    if (wgnx_driver_open_udp(rt->driver_, port) != 0 && wgnx_driver_open_udp(rt->driver_, 0) != 0) {
        *error = "cannot open a UDP socket";
        return nullptr;
    }
    if (!MakeWakePair(&rt->wake_rx_, &rt->wake_tx_)) {
        *error = "cannot create wake sockets";
        return nullptr;
    }
    rt->thread_ = std::thread([self] { self->Loop(nullptr, nullptr); });
    return rt;
}
#endif

std::unique_ptr<Runtime> Runtime::Adopt(WgnxEngine *engine, WgnxDriver *driver, std::string *error) {
    std::unique_ptr<Runtime> rt(new Runtime());
    rt->engine_ = engine;
    rt->driver_ = driver;
    rt->owned_ = false;
    if (!MakeWakePair(&rt->wake_rx_, &rt->wake_tx_)) {
        *error = "cannot create wake sockets";
        return nullptr;
    }
    return rt;
}

void Runtime::Run(const std::function<void(WgnxDriver *)> &on_tick, const std::function<void()> &after_tick) {
    Loop(&on_tick, &after_tick);
}

void Runtime::Stop() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stop_ = true;
        Wake();  // under the lock: the loop may be replacing the pair
    }
    cv_.notify_all();
}

Runtime::~Runtime() {
    Stop();
    if (thread_.joinable()) thread_.join();
    if (owned_) {
        wgnx_driver_free(driver_);
        wgnx_engine_free(engine_);
    }
    if (wake_rx_ >= 0) close(wake_rx_);
    if (wake_tx_ >= 0) close(wake_tx_);
}

void Runtime::Wake() {
    if (wake_tx_ < 0) return;
    char b = 0;
    if (send(wake_tx_, &b, 1, 0) < 0 && SocketGone(errno)) wake_gone_.store(true, std::memory_order_relaxed);
}

void Runtime::AfterCall() {
    if (suspended_) return;  // I/O resumes (and gets pumped) on Resume()
    wgnx_driver_pump(driver_);
    // A wake-up costs the loop a round of socket calls, and most calls (a
    // datagram sent or read, a readiness check) leave its poll as good as
    // it was. A loop that isn't in poll() prepares a new one anyway.
    if (polling_ && wgnx_driver_poll_stale(driver_, poll_until_ns_)) Wake();
}

bool Runtime::Suspend(SuspendReason why, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mu_);
    suspend_reasons_ |= why;
    Wake();
    return cv_.wait_for(lock, timeout, [&] { return suspended_ || stop_; });
}

void Runtime::Resume(SuspendReason why) {
    std::lock_guard<std::mutex> lock(mu_);
    async_suspend_.fetch_and(~static_cast<uint32_t>(why));
    suspend_reasons_ &= ~static_cast<uint32_t>(why);
    cv_.notify_all();
}

void Runtime::RequestSuspended(SuspendReason why, bool suspended) {
    uint32_t cur = async_wanted_.load();
    uint32_t next;
    do {
        next = (cur | (static_cast<uint32_t>(why) << 16)) & ~static_cast<uint32_t>(why);
        if (suspended) next |= why;
    } while (!async_wanted_.compare_exchange_weak(cur, next));
    // No Wake(): the wake pair is the loop's, and it may be closing it.
    cv_.notify_all();
}

// With the lock held (Wake() uses the pair under it): replaces a wake pair
// the system dropped, or one that couldn't be made, once a second at most.
// Without one the loop still runs, with up to a second of latency.
void Runtime::RepairWake(uint64_t now) {
    if (wake_gone_.exchange(false, std::memory_order_relaxed)) {
        // Closed now, so that poll() stops returning at once for it.
        if (wake_rx_ >= 0) close(wake_rx_);
        if (wake_tx_ >= 0) close(wake_tx_);
        wake_rx_ = wake_tx_ = -1;
    }
    if (wake_rx_ >= 0 || now - wake_made_ns_ < kRepairEveryNs) return;
    wake_made_ns_ = now;
    if (MakeWakePair(&wake_rx_, &wake_tx_)) wake_remakes_.fetch_add(1, std::memory_order_relaxed);
}

// With the lock held: reopens the engine's socket if the system dropped it,
// once a second at most, and has the engine contact its peers again.
void Runtime::RepairSocket(uint64_t now) {
    if (!wgnx_driver_broken(driver_) || now - reopened_ns_ < kRepairEveryNs) return;
    reopened_ns_ = now;
    if (wgnx_driver_reopen(driver_) == 0) wgnx_engine_network_up(engine_, wgnx_driver_now_ns());
}

// With the lock held.
void Runtime::ApplyRequests() {
    const uint32_t w = async_wanted_.exchange(0);
    const uint32_t asked = w >> 16;
    suspend_reasons_ = (suspend_reasons_ & ~asked) | (w & asked);
}

void Runtime::WaitForLoop() {
    std::unique_lock<std::mutex> gate(gate_mu_);
    gate_cv_.wait(gate, [&] { return !loop_wants_lock_.load(std::memory_order_acquire); });
}

bool Runtime::WaitFor(const std::function<bool(WgnxEngine *)> &ready, const Clock::time_point *deadline) {
    std::unique_lock<std::mutex> lock(mu_);
    for (;;) {
        if (ready(engine_)) return true;
        if (stop_) return false;
        if (deadline) {
            if (cv_.wait_until(lock, *deadline) == std::cv_status::timeout) return ready(engine_);
        } else {
            cv_.wait(lock);
        }
    }
}

void Runtime::Loop(const std::function<void(WgnxDriver *)> *on_tick, const std::function<void()> *after_tick) {
    InternalScope internal;  // this thread only ever does engine work
    pollfd fds[WGNX_DRIVER_MAX_FDS + 1];
    std::unique_lock<std::mutex> lock(mu_);
    constexpr uint64_t kTickEveryNs = 50'000'000;
    uint64_t last_tick = 0;
    while (!stop_) {
        suspend_reasons_ |= async_suspend_.exchange(0);
        ApplyRequests();
        if (suspend_reasons_ != 0) {
            wgnx_driver_suspend(driver_);
            if (wake_rx_ >= 0) close(wake_rx_);
            if (wake_tx_ >= 0) close(wake_tx_);
            wake_rx_ = wake_tx_ = -1;
            wake_gone_.store(false, std::memory_order_relaxed);  // made anew at resume
            if (on_suspend_) on_suspend_();
            suspended_ = true;
            suspended_flag_ = true;
            cv_.notify_all();
            // RequestSuspended notifies without the lock, so a wake-up can
            // be missed: look again every second.
            while (!stop_) {
                ApplyRequests();
                if (suspend_reasons_ == 0) break;
                cv_.wait_for(lock, std::chrono::seconds(1));
            }
            if (stop_) break;  // stopping: keep everything closed
            // Without a wake pair the loop still runs, just with up to a
            // second of latency for new overlay sockets, and RepairWake
            // tries again.
            if (on_resume_) on_resume_();
            MakeWakePair(&wake_rx_, &wake_tx_);
            wake_made_ns_ = wgnx_driver_now_ns();
            wgnx_driver_resume(driver_);
            wgnx_engine_network_up(engine_, wgnx_driver_now_ns());
            suspended_ = false;
            suspended_flag_ = false;
            cv_.notify_all();
            continue;
        }
        rounds_.fetch_add(1, std::memory_order_relaxed);
        {
            const uint64_t now = wgnx_driver_now_ns();
            RepairWake(now);
            RepairSocket(now);
        }
        // Housekeeping at most every kTickEveryNs: under load the loop runs
        // hundreds of rounds a second, and the embedder's tick (status,
        // logs, heap stats) would hold the lock through every one.
        const uint64_t tick_now = wgnx_driver_now_ns();
        bool ticked = false;
        if (on_tick && *on_tick && tick_now - last_tick >= kTickEveryNs) {
            last_tick = tick_now;
            (*on_tick)(driver_);
            ticked = true;
        }
        int wait = 0;
        int n = wgnx_driver_prepare(driver_, fds, WGNX_DRIVER_MAX_FDS, 1000, &wait);
        fds[n].fd = wake_rx_;
        fds[n].events = POLLIN;
        fds[n].revents = 0;
        const int nfds = n + (wake_rx_ >= 0 ? 1 : 0);
        polling_ = true;
        poll_until_ns_ = wgnx_driver_now_ns() + static_cast<uint64_t>(wait) * 1000000;
        lock.unlock();
        if (ticked && after_tick && *after_tick) (*after_tick)();
        int rc = wgnx_driver_poll(driver_, fds, nfds, wait);
        // Socket reads without the lock: each is an IPC on the Switch, and
        // callers (an app reading video) would otherwise wait them out.
        if (rc > 0) wgnx_driver_prefetch(driver_, fds);
        {
            std::lock_guard<std::mutex> gate(gate_mu_);
            loop_wants_lock_.store(true, std::memory_order_release);
        }
        lock.lock();
        {
            std::lock_guard<std::mutex> gate(gate_mu_);
            loop_wants_lock_.store(false, std::memory_order_release);
        }
        gate_cv_.notify_all();
        polling_ = false;
        // Only this thread replaces the pair (with the lock, which senders
        // hold too), so wake_rx_ is still the one polled.
        if (wake_rx_ >= 0 && (fds[n].revents & (POLLERR | POLLHUP | POLLNVAL))) {
            wake_gone_.store(true, std::memory_order_relaxed);
        } else if (wake_rx_ >= 0 && (fds[n].revents & POLLIN)) {
            char buf[64];
            ssize_t r;
            while ((r = recv(wake_rx_, buf, sizeof buf, 0)) > 0) {
            }
            if (r < 0 && SocketGone(errno)) wake_gone_.store(true, std::memory_order_relaxed);
        }
        if (rc > 0) wgnx_driver_dispatch(driver_, fds);
        // Socket readiness may have changed; let waiters re-check.
        cv_.notify_all();
    }
}

}  // namespace wgnx
