// wgnx-c: host build of the C driver + FFI, for testing on the development
// machine exactly the code paths the Switch sysmodule uses.
//
//   wgnx-c --conf FILE [--port N] [--echo-test <peer ip>] [--sleep-test]
//   wgnx-c --update-check VERSION   (GitHub release check, as at boot)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "wgnx.h"
#include "wgnx_app.h"
#include "wgnx_driver.h"
#include "wgnx_update.h"

void wgnx_platform_random(uint8_t *buf, size_t len) { arc4random_buf(buf, len); }

static const char *arg(int argc, char **argv, const char *name) {
    for (int i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], name)) return argv[i + 1];
    return NULL;
}

static int has(int argc, char **argv, const char *name) {
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], name)) return 1;
    return 0;
}

static int g_handshakes;

static void on_event(void *ctx, const WgnxEvent *ev) {
    (void)ctx;
    static const char *names[] = {"log", "addresses", "peers", "error", "resolve", "handshake", "peer-lost"};
    const char *name = ev->kind < sizeof names / sizeof *names ? names[ev->kind] : "?";
    if (ev->kind == WGNX_EVENT_HANDSHAKE) g_handshakes++;
    printf("[event] %s value=%u %s\n", name, ev->value, ev->text);
    fflush(stdout);
}

static char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    *len = fread(buf, 1, (size_t)n, f);
    buf[*len] = 0;
    fclose(f);
    return buf;
}

// The sysmodule's boot-time update check, with `current` as our version.
static int update_check(const char *current) {
    uint8_t seed[32];
    wgnx_platform_random(seed, sizeof seed);
    wgnx_seed_rng(seed);
    wgnx_set_unix_time((uint64_t)time(NULL));
    WgnxUpdate *u = wgnx_update_start(WGNX_UPDATE_REPO, current, wgnx_driver_now_ns(), 30000);
    if (!u) return 1;
    int r;
    while ((r = wgnx_update_step(u, wgnx_driver_now_ns())) == WGNX_UPDATE_PENDING) usleep(50000);
    if (r == WGNX_UPDATE_FAILED) printf("update check failed: %s\n", wgnx_update_error(u));
    else printf("latest %s: %s\n", wgnx_update_latest(u), r == WGNX_UPDATE_NEWER ? "newer" : "not newer");
    wgnx_update_free(u);
    return r == WGNX_UPDATE_FAILED;
}

int main(int argc, char **argv) {
    const char *update_version = arg(argc, argv, "--update-check");
    if (update_version) return update_check(update_version);
    const char *conf_path = arg(argc, argv, "--conf");
    if (!conf_path) {
        fprintf(stderr, "usage: %s --conf FILE [--port N] [--echo-test IP] [--sleep-test]\n       %s --update-check VERSION\n", argv[0], argv[0]);
        return 2;
    }
    uint8_t seed[32];
    wgnx_platform_random(seed, sizeof seed);
    wgnx_seed_rng(seed);

    size_t conf_len = 0;
    char *conf = read_file(conf_path, &conf_len);
    if (!conf) {
        fprintf(stderr, "cannot read %s\n", conf_path);
        return 1;
    }
    bool corrected = false;
    uint64_t unix_time = wgnx_sane_unix_time((uint64_t)time(NULL), WGNX_UPDATE_HOST, &corrected);
    if (corrected) fprintf(stderr, "warning: system clock implausible; using %s's Date\n", WGNX_UPDATE_HOST);
    if (unix_time == 0) unix_time = (uint64_t)time(NULL);
    WgnxEngine *engine = wgnx_engine_new(wgnx_driver_now_ns(), unix_time);
    WgnxConfig cfg = {.conf = conf, .conf_len = conf_len};
    char err[160];
    if (!wgnx_engine_reload(engine, &cfg, wgnx_driver_now_ns(), err, sizeof err)) {
        fprintf(stderr, "%s: %s\n", conf_path, err);
        return 1;
    }
    free(conf);
    WgnxDriver *driver = wgnx_driver_new(engine, on_event, NULL);
    const char *port = arg(argc, argv, "--port");
    uint16_t udp_port = port ? (uint16_t)atoi(port) : wgnx_engine_listen_port(engine);
    if (wgnx_driver_open_udp(driver, udp_port) != 0 && wgnx_driver_open_udp(driver, 0) != 0) {
        fprintf(stderr, "cannot open a UDP socket\n");
        return 1;
    }

    const char *echo_ip = arg(argc, argv, "--echo-test");
    const int sleep_test = has(argc, argv, "--sleep-test");
    WgnxAddr target;
    if (echo_ip && wgnx_parse_ipv4(echo_ip, 7, &target) != 0) {
        fprintf(stderr, "bad --echo-test address\n");
        return 2;
    }
    WgnxEcho echo;
    int round = 0;  // 0: first echo; 1: after a suspend/resume (--sleep-test)
    uint64_t resumed_ns = 0;
    if (echo_ip) wgnx_echo_init(&echo, &target);
    for (;;) {
        if (wgnx_driver_run_once(driver, 50) != 0) {
            fprintf(stderr, "driver error\n");
            return 1;
        }
        if (!echo_ip) continue;
        int r = wgnx_echo_step(&echo, engine, wgnx_driver_now_ns());
        if (!r) continue;
        if (r < 0) {
            printf("echo test FAILED%s\n", round ? " after resume" : "");
            return 1;
        }
        if (round == 0 && sleep_test) {
            // What the console does around sleep: close the socket, idle,
            // reopen it and contact the peers again.
            printf("echo test PASSED; suspending 5 s\n");
            fflush(stdout);
            wgnx_driver_suspend(driver);
            sleep(5);
            wgnx_driver_resume(driver);
            resumed_ns = wgnx_driver_now_ns();
            wgnx_engine_network_up(engine, resumed_ns);
            wgnx_echo_init(&echo, &target);
            round = 1;
            continue;
        }
        if (round == 1) {
            double secs = (double)(wgnx_driver_now_ns() - resumed_ns) / 1e9;
            printf("echo test PASSED after resume in %.2f s\n", secs);
            return secs < 3.0 ? 0 : 1;
        }
        printf("echo test PASSED\n");
        return 0;
    }
}
