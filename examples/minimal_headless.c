/*
 * minimal_headless.c — smallest possible waywallen_display consumer.
 *
 * Connects to the daemon's `display.sock`, registers a fake 1920×1080
 * display, and drives the dispatch loop until the session disconnects
 * or `--max-frames N` has been reached. All callbacks log to stderr.
 *
 * This example has no GL/Vk — its job is to prove the library can
 * drive a full session against the real Rust daemon and log what
 * flows through. A later phase will add `minimal_egl.c` that actually
 * imports the dma-buf handles and renders them.
 *
 * Usage:
 *   minimal_headless [--socket PATH] [--name STR]
 *                    [--width W] [--height H] [--refresh-mhz MHZ]
 *                    [--max-frames N]
 */

#include "waywallen_display.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct args {
    const char *socket_path;
    const char *name;
    uint32_t width;
    uint32_t height;
    uint32_t refresh_mhz;
    int64_t max_frames;
};

struct run_state {
    int64_t frames_seen;
    int64_t max_frames;
    int disconnected;
    uint32_t last_buffer_index;
    uint64_t last_seq;
};

static void on_textures_ready(void *ud, const waywallen_textures_t *t) {
    (void)ud;
    fprintf(stderr,
        "[cb] textures_ready: count=%u tex=%ux%u fourcc=0x%08x mod=0x%016lx planes=%u backend=%d\n",
        t->count, t->tex_width, t->tex_height, t->fourcc,
        (unsigned long)t->modifier, t->planes_per_buffer, (int)t->backend);
}

static void on_textures_releasing(void *ud, const waywallen_textures_t *t) {
    (void)ud;
    fprintf(stderr, "[cb] textures_releasing: count=%u\n", t->count);
}

static void on_config(void *ud, const waywallen_config_t *c) {
    (void)ud;
    fprintf(stderr,
        "[cb] config: source=(%.0f,%.0f,%.0f,%.0f) dest=(%.0f,%.0f,%.0f,%.0f) xform=%u\n",
        (double)c->source_rect.x, (double)c->source_rect.y,
        (double)c->source_rect.w, (double)c->source_rect.h,
        (double)c->dest_rect.x, (double)c->dest_rect.y,
        (double)c->dest_rect.w, (double)c->dest_rect.h,
        c->transform);
}

static void on_frame_ready(void *ud, const waywallen_frame_t *f) {
    struct run_state *rs = (struct run_state *)ud;
    rs->frames_seen++;
    rs->last_buffer_index = f->buffer_index;
    rs->last_seq = f->seq;
    fprintf(stderr,
        "[cb] frame_ready #%lld: idx=%u seq=%llu\n",
        (long long)rs->frames_seen, f->buffer_index, (unsigned long long)f->seq);
    // No GPU work in this demo, so signal the release_syncobj
    // immediately. Otherwise the daemon's reaper waits 500ms per frame
    // and force-signals with a warning. The helper closes the fd.
    if (f->release_syncobj_fd >= 0) {
        (void)waywallen_display_signal_release_syncobj(
            f->release_syncobj_fd);
    }
}

static void on_disconnected(void *ud, int code, const char *msg) {
    struct run_state *rs = (struct run_state *)ud;
    rs->disconnected = 1;
    fprintf(stderr, "[cb] disconnected: code=%d msg=%s\n",
            code, msg ? msg : "(null)");
}

static void usage(void) {
    fprintf(stderr,
        "usage: minimal_headless [--socket PATH] [--name STR] "
        "[--width W] [--height H] [--refresh-mhz MHZ] [--max-frames N]\n");
}

static int parse_u32(const char *s, uint32_t *out) {
    char *end;
    unsigned long v = strtoul(s, &end, 10);
    if (*end != '\0' || v > 0xffffffffUL) return -1;
    *out = (uint32_t)v;
    return 0;
}

static int parse_i64(const char *s, int64_t *out) {
    char *end;
    long long v = strtoll(s, &end, 10);
    if (*end != '\0') return -1;
    *out = (int64_t)v;
    return 0;
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);

    struct args a = {
        .socket_path = NULL,
        .name = "minimal-headless",
        .width = 1920,
        .height = 1080,
        .refresh_mhz = 60000,
        .max_frames = -1,
    };

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if ((strcmp(arg, "--socket") == 0 || strcmp(arg, "--display-sock") == 0)
            && i + 1 < argc) {
            a.socket_path = argv[++i];
        } else if (strcmp(arg, "--name") == 0 && i + 1 < argc) {
            a.name = argv[++i];
        } else if (strcmp(arg, "--width") == 0 && i + 1 < argc) {
            if (parse_u32(argv[++i], &a.width) < 0) { usage(); return 2; }
        } else if (strcmp(arg, "--height") == 0 && i + 1 < argc) {
            if (parse_u32(argv[++i], &a.height) < 0) { usage(); return 2; }
        } else if (strcmp(arg, "--refresh-mhz") == 0 && i + 1 < argc) {
            if (parse_u32(argv[++i], &a.refresh_mhz) < 0) { usage(); return 2; }
        } else if (strcmp(arg, "--max-frames") == 0 && i + 1 < argc) {
            if (parse_i64(argv[++i], &a.max_frames) < 0) { usage(); return 2; }
        } else if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            usage();
            return 0;
        } else {
            fprintf(stderr, "ignoring unknown arg: %s\n", arg);
        }
    }

    struct run_state rs = {
        .frames_seen = 0,
        .max_frames = a.max_frames,
        .disconnected = 0,
        .last_buffer_index = 0,
        .last_seq = 0,
    };

    waywallen_display_callbacks_t cb = {
        .on_textures_ready = on_textures_ready,
        .on_textures_releasing = on_textures_releasing,
        .on_config = on_config,
        .on_frame_ready = on_frame_ready,
        .on_disconnected = on_disconnected,
        .user_data = &rs,
    };

    waywallen_display_t *d = waywallen_display_new(&cb);
    if (!d) {
        fprintf(stderr, "new failed\n");
        return 1;
    }

    fprintf(stderr, "connecting to %s...\n",
            a.socket_path ? a.socket_path : "$XDG_RUNTIME_DIR/waywallen/display.sock");

    /* Demonstrates event-loop-friendly handshake. For a one-shot CLI
     * the legacy `waywallen_display_connect()` would do the same
     * (blocking) thing in a single call. */
    int rc = waywallen_display_begin_connect(d, a.socket_path, a.name,
                                             a.width, a.height, a.refresh_mhz);
    if (rc != WAYWALLEN_OK) {
        fprintf(stderr, "begin_connect failed: %d\n", rc);
        waywallen_display_destroy(d);
        return 1;
    }
    int fd = waywallen_display_get_fd(d);
    while (1) {
        rc = waywallen_display_advance_handshake(d);
        if (rc == WAYWALLEN_HS_DONE) break;
        if (rc < 0) {
            fprintf(stderr, "handshake failed: %d\n", rc);
            waywallen_display_destroy(d);
            return 1;
        }
        struct pollfd hp = { .fd = fd, .events = 0, .revents = 0 };
        if (rc == WAYWALLEN_HS_NEED_READ)       hp.events = POLLIN;
        else if (rc == WAYWALLEN_HS_NEED_WRITE) hp.events = POLLOUT;
        else                                    hp.events = POLLIN | POLLOUT;
        int pn = poll(&hp, 1, -1);
        if (pn < 0 && errno != EINTR) { perror("poll handshake"); return 1; }
    }
    fprintf(stderr, "connected; draining events...\n");
    while (!rs.disconnected) {
        struct pollfd p = { .fd = fd, .events = POLLIN, .revents = 0 };
        int pr = poll(&p, 1, -1);
        if (pr < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }
        if (p.revents & (POLLERR | POLLHUP)) {
            fprintf(stderr, "poll: peer closed (revents=0x%x)\n", p.revents);
            break;
        }
        if (p.revents & POLLIN) {
            int r = waywallen_display_dispatch(d);
            if (r < 0) {
                /* on_disconnected has fired */
                break;
            }
            /* Host responsibility: release each frame after GPU work
             * completes. Phase 2 skips GPU work entirely, so release
             * immediately. */
            if (rs.frames_seen > 0) {
                (void)waywallen_display_release_frame(
                    d, rs.last_buffer_index, rs.last_seq);
            }
            if (rs.max_frames >= 0 && rs.frames_seen >= rs.max_frames) {
                fprintf(stderr, "max-frames reached\n");
                break;
            }
        }
    }

    waywallen_display_disconnect(d);
    waywallen_display_destroy(d);
    return 0;
}
