// Tests for the runtime's non-blocking pause, RequestSuspended, and its
// recovery from sockets the system drops (make runtime-test).
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cassert>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <thread>

#include "runtime.hpp"

extern "C" void wgnx_platform_random(uint8_t *buf, size_t len);

using wgnx::Runtime;

static bool WaitSuspended(Runtime &rt, bool want) {
    for (int i = 0; i < 30; i++) {
        if (rt.IsSuspended() == want) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

template <class F>
static bool Eventually(F &&f, int ms) {
    for (int i = 0; i < ms / 20; i++) {
        if (f()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return f();
}

// The descriptor of a socket bound to this local port, or -1.
static int FindBound(uint16_t port) {
    for (int fd = 3; fd < 256; fd++) {
        sockaddr_in a{};
        socklen_t len = sizeof a;
        if (getsockname(fd, reinterpret_cast<sockaddr *>(&a), &len) == 0 && a.sin_family == AF_INET && ntohs(a.sin_port) == port)
            return fd;
    }
    return -1;
}

// The runtime's wake pair: the loopback socket another loopback socket is
// connected to. Returns its receiving end, or -1.
static int FindWakeRx() {
    for (int fd = 3; fd < 256; fd++) {
        sockaddr_in peer{};
        socklen_t len = sizeof peer;
        if (getpeername(fd, reinterpret_cast<sockaddr *>(&peer), &len) != 0 || peer.sin_family != AF_INET ||
            peer.sin_addr.s_addr != htonl(INADDR_LOOPBACK))
            continue;
        const int rx = FindBound(ntohs(peer.sin_port));
        if (rx >= 0 && rx != fd) return rx;
    }
    return -1;
}

// The driver on its own: a socket the system dropped is found by a send and
// by poll(), no longer polled, and reopened on the same port.
static void DriverReopens(const WgnxConfig &cfg) {
    char err[128];
    WgnxEngine *engine = wgnx_engine_new(wgnx_driver_now_ns(), static_cast<uint64_t>(time(nullptr)));
    assert(wgnx_engine_reload(engine, &cfg, wgnx_driver_now_ns(), err, sizeof err));
    WgnxDriver *d = wgnx_driver_new(engine, nullptr, nullptr);
    assert(wgnx_driver_open_udp(d, 0) == 0);
    const uint16_t port = wgnx_driver_udp_port(d);
    pollfd fds[WGNX_DRIVER_MAX_FDS];
    int wait;
    assert(!wgnx_driver_broken(d));

    // Found by a send (the first handshake, sent by prepare), and then not
    // polled.
    close(FindBound(port));
    assert(wgnx_driver_prepare(d, fds, WGNX_DRIVER_MAX_FDS, 0, &wait) == 0);
    assert(wgnx_driver_broken(d));
    assert(wgnx_driver_reopen(d) == 0);
    int gone = 0;
    assert(!wgnx_driver_broken(d) && wgnx_driver_udp_port(d) == port && wgnx_driver_reopens(d, &gone) == 1 && gone == EBADF);

    // Found by poll() (it reports the descriptor invalid).
    assert(wgnx_driver_prepare(d, fds, WGNX_DRIVER_MAX_FDS, 0, &wait) == 1);
    close(fds[0].fd);
    assert(wgnx_driver_poll(d, fds, 1, 100) == 1 && (fds[0].revents & POLLNVAL));
    wgnx_driver_prefetch(d, fds);
    wgnx_driver_dispatch(d, fds);
    assert(wgnx_driver_broken(d));
    assert(wgnx_driver_reopen(d) == 0);
    assert(wgnx_driver_udp_port(d) == port && wgnx_driver_reopens(d, &gone) == 2 && gone == -1);

    // Reopened when nothing found it gone (the network came back).
    assert(wgnx_driver_reopen(d) == 0 && wgnx_driver_reopens(d, &gone) == 3 && gone == 0 && wgnx_driver_udp_port(d) == port);

    // Found by poll() reporting it readable round after round with nothing
    // to read and no error (Airplane Mode did that for ~6 s). A UDP socket
    // shut for reading stands in: always readable, every read empty. Both
    // ways the loop reads: prefetch then dispatch, and dispatch alone.
    for (const bool prefetch : {true, false}) {
        assert(wgnx_driver_prepare(d, fds, WGNX_DRIVER_MAX_FDS, 0, &wait) == 1);
        const int empty = socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port = htons(9);
        to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        assert(empty >= 0 && connect(empty, reinterpret_cast<sockaddr *>(&to), sizeof to) == 0 && shutdown(empty, SHUT_RD) == 0);
        assert(dup2(empty, fds[0].fd) == fds[0].fd);
        close(empty);
        int rounds = 0;
        while (!wgnx_driver_broken(d) && rounds < 1000) {
            assert(wgnx_driver_prepare(d, fds, WGNX_DRIVER_MAX_FDS, 0, &wait) == 1);
            assert(wgnx_driver_poll(d, fds, 1, 100) == 1 && (fds[0].revents & POLLIN));
            if (prefetch) wgnx_driver_prefetch(d, fds);
            wgnx_driver_dispatch(d, fds);
            rounds++;
        }
        assert(rounds == 64);  // UDP_EMPTY_READY_LIMIT
        assert(wgnx_driver_prepare(d, fds, WGNX_DRIVER_MAX_FDS, 0, &wait) == 0);  // not polled
        const unsigned int before = wgnx_driver_reopens(d, nullptr);
        assert(wgnx_driver_reopen(d) == 0 && wgnx_driver_reopens(d, &gone) == before + 1 && gone == -2 && wgnx_driver_udp_port(d) == port);
    }

    // A sleep's suspend is not a broken socket.
    wgnx_driver_suspend(d);
    assert(!wgnx_driver_broken(d));
    wgnx_driver_free(d);
    wgnx_engine_free(engine);
    std::puts("ok: a dropped socket is found (send, poll, empty reads), not polled, and reopened on the same port");
}

int main() {
    uint8_t seed[32];
    wgnx_platform_random(seed, 32);
    wgnx_seed_rng(seed);
    // A peer nobody answers at: the engine just keeps trying to handshake.
    // Its keepalive is shorter than the 5 s handshake retries, which once
    // kept the loop busy (the round counts below would show it).
    static const char kConf[] =
        "[Interface]\nPrivateKey = yAnz5TF+lXXJte14tji3zlMNq+hd2rYUIgJBgB3fBmk=\nAddress = 10.7.0.2/32\n"
        "[Peer]\nPublicKey = xTIBA5rboUvnH4htodjb6e697QjLERt1NAB4mZqp8Dg=\nAllowedIPs = 10.7.0.1/32\n"
        "Endpoint = 127.0.0.1:9\nPersistentKeepalive = 1\n";
    WgnxEngine *engine = wgnx_engine_new(wgnx_driver_now_ns(), static_cast<uint64_t>(time(nullptr)));
    WgnxConfig cfg{kConf, sizeof kConf - 1};
    char err[128];
    assert(wgnx_engine_reload(engine, &cfg, wgnx_driver_now_ns(), err, sizeof err));
    DriverReopens(cfg);
    WgnxDriver *driver = wgnx_driver_new(engine, nullptr, nullptr);
    assert(wgnx_driver_open_udp(driver, 0) == 0);
    std::string error;
    auto rt = Runtime::Adopt(engine, driver, &error);
    assert(rt);
    std::thread loop([&] { rt->Run([](WgnxDriver *) {}); });

    rt->RequestSuspended(Runtime::kPaused, true);
    assert(WaitSuspended(*rt, true));
    rt->RequestSuspended(Runtime::kPaused, false);
    assert(WaitSuspended(*rt, false));

    // The latest request wins, however quickly they follow each other.
    rt->RequestSuspended(Runtime::kPaused, true);
    rt->RequestSuspended(Runtime::kPaused, false);
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    assert(!rt->IsSuspended());
    rt->RequestSuspended(Runtime::kPaused, false);
    rt->RequestSuspended(Runtime::kPaused, true);
    assert(WaitSuspended(*rt, true));

    // Reasons stack: resuming the pause during a sleep stays suspended
    // until the wake.
    rt->RequestSuspend(Runtime::kSleep);
    rt->RequestSuspended(Runtime::kPaused, false);
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    assert(rt->IsSuspended());
    rt->Resume(Runtime::kSleep);
    assert(WaitSuspended(*rt, false));
    assert(wgnx_driver_udp_port(driver) != 0);  // reopened on resume

    rt->Resume(Runtime::kSleep);
    rt->RequestSuspended(Runtime::kPaused, false);
    assert(WaitSuspended(*rt, false));

    // The system drops the engine's socket (Airplane Mode on the console):
    // the loop reopens it on the same port, without spinning meanwhile.
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const uint16_t port = wgnx_driver_udp_port(driver);
        const int fd = FindBound(port);
        assert(fd >= 0);
        const uint64_t rounds = rt->Rounds();
        close(fd);
        assert(Eventually([&] { return wgnx_driver_reopens(driver, nullptr) == 1; }, 3000));
        assert(Eventually([&] { return FindBound(port) >= 0; }, 1000));
        const uint64_t spent = rt->Rounds() - rounds;
        assert(spent < 50);
        std::printf("ok: a dropped engine socket is reopened on port %u (%llu rounds meanwhile)\n", port,
                    static_cast<unsigned long long>(spent));
    }

    // ... and the loop's wake pair: replaced, and wake-ups work again.
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const int rx = FindWakeRx();
        assert(rx >= 0);
        const uint64_t rounds = rt->Rounds();
        close(rx);
        assert(Eventually([&] { return rt->WakeRemakes() == 1; }, 3000));
        const uint64_t spent = rt->Rounds() - rounds;
        assert(spent < 50);
        assert(FindWakeRx() >= 0);
        // Suspend() wakes the loop out of its poll (up to a second long here:
        // the peer never answers): with a working pair it suspends at once.
        for (int i = 0; i < 3; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            assert(rt->Suspend(Runtime::kPaused, std::chrono::milliseconds(150)));
            rt->Resume(Runtime::kPaused);
            assert(WaitSuspended(*rt, false));
        }
        std::printf("ok: a dropped wake pair is remade (%llu rounds meanwhile)\n", static_cast<unsigned long long>(spent));
    }

    rt->Stop();
    loop.join();
    rt.reset();
    wgnx_driver_free(driver);
    wgnx_engine_free(engine);
    std::puts("runtime tests passed");
    return 0;
}
