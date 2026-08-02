/*
 * End-to-end smoke tests for `libwaywallen_display` at the handshake
 * layer.
 *
 * Each test pairs a tiny mock server (spun up in a background pthread
 * on a fresh tempfile UDS) with a client driven through the public
 * API, walking the `hello` → `welcome` → `register_display` →
 * `display_accepted` exchange via the same generated `ww_proto`
 * encoders the Rust daemon would use.
 *
 * Coverage:
 *   1. test_connect_to_nonexistent_socket    — begin_connect doesn't block on a missing peer
 *   2. test_legacy_blocking_connect          — old `waywallen_display_connect` still works (poll
 * wrapper)
 *   3. test_begin_connect_immediate          — begin_connect transitions to HELLO_PENDING on sync
 * accept
 *   4. test_full_handshake_via_async_api     — full begin → advance × N → DONE round-trip
 *   5. test_partial_welcome                  — welcome split across multiple kernel writes
 *   6. test_server_closes_during_welcome_wait — peer EOF mid-handshake → on_disconnected fires
 *   7. test_server_sends_error_event         — server sends WW_EVT_ERROR before welcome
 */

#define _POSIX_C_SOURCE 200809L

#include "waywallen_display.h"

#include "codec.h"
#include "ww_proto.h"

#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static void sleep_ms(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* ------------------------------------------------------------------ */
/*  Test harness                                                       */
/* ------------------------------------------------------------------ */

struct test_state {
    char sock_path[128];
    int  listen_fd;

    int on_disconnected_count;
    int on_binding_ready_count;
    int on_textures_releasing_count;
    int on_composition_config_count;
    int on_frame_ready_count;
    int on_presentation_snapshot_count;
    int on_presentation_state_count;

    int  last_disconnect_code;
    char last_disconnect_msg[256];

    uint64_t                               last_binding_buffer_generation;
    uint64_t                               last_composition_buffer_generation;
    uint64_t                               last_composition_generation;
    uint64_t                               last_frame_buffer_generation;
    uint64_t                               last_release_armed_buffer_generation;
    uint64_t                               last_release_armed_seq;
    uint64_t                               last_ack_unbind_generation;
    uint64_t                               last_import_failure_generation;
    waywallen_buffer_import_failure_kind_t last_import_failure_kind;
    waywallen_presentation_snapshot_t      last_presentation;

    uint32_t outbox_axis_count;
    uint32_t outbox_motion_count;
    uint32_t outbox_metrics_count;
    uint32_t outbox_window_count;
    uint32_t outbox_requests_before_critical;
    float    outbox_last_motion_x;
    uint32_t outbox_last_metrics_width;
    uint32_t outbox_last_window_flags;
    int      outbox_saw_critical;

    /* Populated from the atomic register_display request. */
    int                         saw_consumer_caps;
    uint32_t                    consumer_caps_mem_hints;
    uint32_t                    consumer_caps_sync_caps;
    uint32_t                    consumer_caps_color_caps;
    uint32_t                    presentation_caps;
    waywallen_display_metrics_t registered_metrics;
    uint32_t                    registered_window_state;
};

static void cb_binding_ready(void* ud, const waywallen_binding_t* binding) {
    struct test_state* ts = (struct test_state*)ud;
    ts->on_binding_ready_count++;
    const waywallen_textures_t* t          = &binding->textures;
    ts->last_binding_buffer_generation     = t->buffer_generation;
    ts->last_composition_buffer_generation = binding->config.buffer_generation;
    ts->last_composition_generation        = binding->config.generation;
}
static void cb_textures_releasing(void* ud, const waywallen_textures_t* t) {
    struct test_state* ts = (struct test_state*)ud;
    ts->on_textures_releasing_count++;
    (void)t;
}
static void cb_composition_config(void* ud, const waywallen_composition_config_t* c) {
    struct test_state* ts = (struct test_state*)ud;
    ts->on_composition_config_count++;
    ts->last_composition_buffer_generation = c->buffer_generation;
    ts->last_composition_generation        = c->generation;
}
static void cb_frame_ready(void* ud, const waywallen_frame_t* f) {
    struct test_state* ts = (struct test_state*)ud;
    ts->on_frame_ready_count++;
    ts->last_frame_buffer_generation = f->buffer_generation;
    if (f->release_syncobj_fd >= 0) close(f->release_syncobj_fd);
}
static void cb_presentation_snapshot(void*                                    ud,
                                     const waywallen_presentation_snapshot_t* presentation) {
    struct test_state* ts = (struct test_state*)ud;
    ts->on_presentation_snapshot_count++;
    ts->last_presentation = *presentation;
}
static void cb_presentation_state(void* ud, const waywallen_presentation_state_t* state) {
    struct test_state* ts = (struct test_state*)ud;
    ts->on_presentation_state_count++;
    ts->last_presentation.state = *state;
}
static void cb_disconnected(void* ud, int code, const char* msg) {
    struct test_state* ts = (struct test_state*)ud;
    ts->on_disconnected_count++;
    ts->last_disconnect_code = code;
    if (msg) {
        snprintf(ts->last_disconnect_msg, sizeof(ts->last_disconnect_msg), "%s", msg);
    } else {
        ts->last_disconnect_msg[0] = '\0';
    }
}

static const waywallen_display_callbacks_t kCallbacks = {
    .on_binding_ready         = cb_binding_ready,
    .on_textures_releasing    = cb_textures_releasing,
    .on_composition_config    = cb_composition_config,
    .on_frame_ready           = cb_frame_ready,
    .on_presentation_snapshot = cb_presentation_snapshot,
    .on_presentation_state    = cb_presentation_state,
    .on_disconnected          = cb_disconnected,
    .user_data                = NULL,
};

static ww_evt_display_accepted_t accepted_event(uint64_t display_id) {
    ww_evt_display_accepted_t accepted                    = { 0 };
    accepted.display_id                                   = display_id;
    accepted.presentation.config.generation               = 1;
    accepted.presentation.config.pause_effect.kind        = WAYWALLEN_PAUSE_EFFECT_KIND_NONE;
    accepted.presentation.config.pause_effect.blur.radius = 30;
    accepted.presentation.state.generation                = 1;
    accepted.presentation.state.config_generation         = 1;
    accepted.presentation.state.pause_effect.active       = false;
    return accepted;
}

static void ts_init(struct test_state* ts) {
    memset(ts, 0, sizeof(*ts));
    snprintf(ts->sock_path,
             sizeof(ts->sock_path),
             "/tmp/waywallen-test-display-%d-%p.sock",
             (int)getpid(),
             (void*)ts);
    unlink(ts->sock_path);

    ts->listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    assert(ts->listen_fd >= 0);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    size_t pl       = strlen(ts->sock_path);
    assert(pl < sizeof(addr.sun_path));
    memcpy(addr.sun_path, ts->sock_path, pl);
    assert(bind(ts->listen_fd, (struct sockaddr*)&addr, sizeof(addr)) == 0);
    assert(listen(ts->listen_fd, 1) == 0);
}

static void ts_teardown(struct test_state* ts) {
    if (ts->listen_fd >= 0) {
        close(ts->listen_fd);
        ts->listen_fd = -1;
    }
    unlink(ts->sock_path);
}

typedef int (*server_handler_t)(int client_fd, struct test_state* ts);

struct server_thread_arg {
    struct test_state* ts;
    server_handler_t   handler;
};

static void* server_thread_fn(void* arg) {
    struct server_thread_arg* sta = (struct server_thread_arg*)arg;
    struct sockaddr_un        peer;
    socklen_t                 peer_len = sizeof(peer);
    int client_fd = accept(sta->ts->listen_fd, (struct sockaddr*)&peer, &peer_len);
    if (client_fd < 0) {
        fprintf(stderr, "[server] accept: %s\n", strerror(errno));
        free(sta);
        return NULL;
    }
    (void)sta->handler(client_fd, sta->ts);
    close(client_fd);
    free(sta);
    return NULL;
}

static pthread_t spawn_server(struct test_state* ts, server_handler_t handler) {
    struct server_thread_arg* sta = (struct server_thread_arg*)calloc(1, sizeof(*sta));
    sta->ts                       = ts;
    sta->handler                  = handler;
    pthread_t tid;
    assert(pthread_create(&tid, NULL, server_thread_fn, sta) == 0);
    return tid;
}

static waywallen_display_t* make_client(struct test_state* ts) {
    waywallen_display_callbacks_t cb = kCallbacks;
    cb.user_data                     = ts;
    waywallen_display_t* d           = waywallen_display_new(&cb);
    assert(d != NULL);
    return d;
}

static int begin_test_display(waywallen_display_t* d, const char* socket_path, uint32_t width,
                              uint32_t height) {
    const waywallen_display_metrics_t metrics = { .width       = width,
                                                  .height      = height,
                                                  .refresh_mhz = 60000 };
    return waywallen_display_begin_connect(d, socket_path, "test-display", NULL, &metrics);
}

static int connect_test_display(waywallen_display_t* d, const char* socket_path, uint32_t width,
                                uint32_t height) {
    const waywallen_display_metrics_t metrics = { .width       = width,
                                                  .height      = height,
                                                  .refresh_mhz = 60000 };
    return waywallen_display_connect(d, socket_path, "test-display", NULL, &metrics);
}

/* Drive begin_connect → advance_handshake to completion via a private
 * poll loop. Returns WAYWALLEN_OK on DONE, or the error code from the
 * state machine. Times out at `timeout_ms`. */
static int drive_handshake(waywallen_display_t* d, int timeout_ms) {
    int fd = waywallen_display_get_fd(d);
    for (;;) {
        int rc = waywallen_display_advance_handshake(d);
        if (rc == WAYWALLEN_HS_DONE) return WAYWALLEN_OK;
        if (rc < 0) return rc;
        struct pollfd pfd;
        pfd.fd      = fd;
        pfd.revents = 0;
        if (rc == WAYWALLEN_HS_NEED_READ)
            pfd.events = POLLIN;
        else if (rc == WAYWALLEN_HS_NEED_WRITE)
            pfd.events = POLLOUT;
        else
            pfd.events = POLLIN | POLLOUT;
        int n = poll(&pfd, 1, timeout_ms);
        if (n == 0) return -ETIMEDOUT;
        if (n < 0) {
            if (errno == EINTR) continue;
            return -errno;
        }
    }
}

static int dispatch_next_event(waywallen_display_t* d);

static void capture_registration(struct test_state* ts, const ww_req_register_display_t* reg) {
    ts->presentation_caps        = reg->presentation_caps.flags;
    ts->consumer_caps_mem_hints  = reg->consumer_caps.mem_hints;
    ts->consumer_caps_sync_caps  = reg->consumer_caps.sync_caps;
    ts->consumer_caps_color_caps = reg->consumer_caps.color_caps;
    ts->registered_metrics       = reg->metrics;
    ts->registered_window_state  = reg->window_state_flags;
    ts->saw_consumer_caps        = 1;
}

/* ------------------------------------------------------------------ */
/*  Mock server handlers                                               */
/* ------------------------------------------------------------------ */

/* Full happy-path handshake responder; mirrors what the daemon does. */
static int handler_full_handshake(int client_fd, struct test_state* ts) {
    (void)ts;
    static uint8_t body_buf[WW_CODEC_MAX_BODY_BYTES];
    uint16_t       op;
    size_t         body_len;
    int            fds[4];
    size_t         n_fds;

    int rc = ww_codec_recv_request(
        client_fd, &op, body_buf, WW_CODEC_MAX_BODY_BYTES, &body_len, fds, 4, &n_fds);
    if (rc != 0 || op != WW_REQ_HELLO) return -1;
    ww_req_hello_t hello;
    if (ww_req_hello_decode(body_buf, body_len, &hello) != WW_OK) return -1;
    ww_req_hello_free(&hello);

    /* Send WELCOME. */
    ww_evt_welcome_t welcome;
    memset(&welcome, 0, sizeof(welcome));
    welcome.server_version = (char*)"mock-server/0.1";
    ww_buf_t out;
    ww_buf_init(&out);
    if (ww_evt_welcome_encode(&welcome, &out) != WW_OK) {
        ww_buf_free(&out);
        return -1;
    }
    rc = ww_codec_send_event(client_fd, WW_EVT_WELCOME, out.data, out.len, NULL, 0);
    ww_buf_free(&out);
    if (rc != 0) return -1;

    /* Expect REGISTER_DISPLAY. */
    rc = ww_codec_recv_request(
        client_fd, &op, body_buf, WW_CODEC_MAX_BODY_BYTES, &body_len, fds, 4, &n_fds);
    if (rc != 0 || op != WW_REQ_REGISTER_DISPLAY) return -1;
    ww_req_register_display_t reg;
    if (ww_req_register_display_decode(body_buf, body_len, &reg) != WW_OK) return -1;
    capture_registration(ts, &reg);
    ww_req_register_display_free(&reg);

    /* Send DISPLAY_ACCEPTED. */
    ww_evt_display_accepted_t accepted = accepted_event(42);
    ww_buf_init(&out);
    if (ww_evt_display_accepted_encode(&accepted, &out) != WW_OK) {
        ww_buf_free(&out);
        return -1;
    }
    rc = ww_codec_send_event(client_fd, WW_EVT_DISPLAY_ACCEPTED, out.data, out.len, NULL, 0);
    ww_buf_free(&out);
    if (rc != 0) return -1;

    /* Wait for peer close. */
    rc = ww_codec_recv_request(
        client_fd, &op, body_buf, WW_CODEC_MAX_BODY_BYTES, &body_len, fds, 4, &n_fds);
    if (rc == 0 && op == WW_REQ_SET_DISPLAY_METRICS) {
        ww_req_set_display_metrics_t metrics;
        if (ww_req_set_display_metrics_decode(body_buf, body_len, &metrics) == WW_OK) {
            ts->registered_metrics = metrics.metrics;
            ww_req_set_display_metrics_free(&metrics);
        }
    }
    return 0;
}

/* Complete a handshake while retaining the atomic registration facts. */
static int complete_handshake_capture_caps(int client_fd, struct test_state* ts) {
    static uint8_t body_buf[WW_CODEC_MAX_BODY_BYTES];
    uint16_t       op;
    size_t         body_len;
    int            fds[4];
    size_t         n_fds;

    /* HELLO -> WELCOME */
    int rc = ww_codec_recv_request(
        client_fd, &op, body_buf, WW_CODEC_MAX_BODY_BYTES, &body_len, fds, 4, &n_fds);
    if (rc != 0 || op != WW_REQ_HELLO) return -1;
    ww_req_hello_t hello;
    if (ww_req_hello_decode(body_buf, body_len, &hello) != WW_OK) return -1;
    ww_req_hello_free(&hello);

    ww_evt_welcome_t welcome;
    memset(&welcome, 0, sizeof(welcome));
    welcome.server_version = (char*)"mock-server/0.1";
    ww_buf_t out;
    ww_buf_init(&out);
    if (ww_evt_welcome_encode(&welcome, &out) != WW_OK) {
        ww_buf_free(&out);
        return -1;
    }
    rc = ww_codec_send_event(client_fd, WW_EVT_WELCOME, out.data, out.len, NULL, 0);
    ww_buf_free(&out);
    if (rc != 0) return -1;

    /* REGISTER_DISPLAY -> DISPLAY_ACCEPTED */
    rc = ww_codec_recv_request(
        client_fd, &op, body_buf, WW_CODEC_MAX_BODY_BYTES, &body_len, fds, 4, &n_fds);
    if (rc != 0 || op != WW_REQ_REGISTER_DISPLAY) return -1;
    ww_req_register_display_t reg;
    if (ww_req_register_display_decode(body_buf, body_len, &reg) != WW_OK) return -1;
    capture_registration(ts, &reg);
    ww_req_register_display_free(&reg);

    ww_evt_display_accepted_t accepted = accepted_event(99);
    ww_buf_init(&out);
    if (ww_evt_display_accepted_encode(&accepted, &out) != WW_OK) {
        ww_buf_free(&out);
        return -1;
    }
    rc = ww_codec_send_event(client_fd, WW_EVT_DISPLAY_ACCEPTED, out.data, out.len, NULL, 0);
    ww_buf_free(&out);
    if (rc != 0) return -1;

    return 0;
}

static int handler_full_handshake_capture_caps(int client_fd, struct test_state* ts) {
    return complete_handshake_capture_caps(client_fd, ts);
}

static int handler_frame_release_armed(int client_fd, struct test_state* ts) {
    if (complete_handshake_capture_caps(client_fd, ts) != 0) return -1;
    static uint8_t body_buf[WW_CODEC_MAX_BODY_BYTES];
    uint16_t       op;
    size_t         body_len;
    int            fds[1];
    size_t         n_fds;
    if (ww_codec_recv_request(
            client_fd, &op, body_buf, sizeof(body_buf), &body_len, fds, 1, &n_fds) != 0 ||
        op != WW_REQ_FRAME_RELEASE_ARMED || n_fds != 0) {
        return -1;
    }
    ww_req_frame_release_armed_t armed;
    if (ww_req_frame_release_armed_decode(body_buf, body_len, &armed) != WW_OK) return -1;
    ts->last_release_armed_buffer_generation = armed.buffer_generation;
    ts->last_release_armed_seq               = armed.seq;
    ww_req_frame_release_armed_free(&armed);
    return 0;
}

static int receive_ack_unbind(int client_fd, struct test_state* ts) {
    uint8_t  body_buf[128];
    uint16_t op;
    size_t   body_len;
    int      fds[1];
    size_t   n_fds;
    if (ww_codec_recv_request(
            client_fd, &op, body_buf, sizeof(body_buf), &body_len, fds, 1, &n_fds) != 0 ||
        op != WW_REQ_ACK_UNBIND || n_fds != 0) {
        return -1;
    }
    ww_req_ack_unbind_t ack;
    if (ww_req_ack_unbind_decode(body_buf, body_len, &ack) != WW_OK) return -1;
    ts->last_ack_unbind_generation = ack.buffer_generation;
    ww_req_ack_unbind_free(&ack);
    return 0;
}

static int receive_import_failure(int client_fd, struct test_state* ts) {
    uint8_t  body_buf[256];
    uint16_t op;
    size_t   body_len;
    int      fds[1];
    size_t   n_fds;
    if (ww_codec_recv_request(
            client_fd, &op, body_buf, sizeof(body_buf), &body_len, fds, 1, &n_fds) != 0 ||
        op != WW_REQ_BUFFER_IMPORT_FAILED || n_fds != 0) {
        return -1;
    }
    ww_req_buffer_import_failed_t failed;
    if (ww_req_buffer_import_failed_decode(body_buf, body_len, &failed) != WW_OK) return -1;
    ts->last_import_failure_generation = failed.buffer_generation;
    ts->last_import_failure_kind       = failed.kind;
    ww_req_buffer_import_failed_free(&failed);
    return 0;
}

static int send_bind_buffers(int client_fd, uint64_t buffer_generation,
                             uint64_t composition_generation) {
    int dmabuf[2];
    if (pipe(dmabuf) != 0) return -1;

    uint32_t stride       = 256;
    uint32_t plane_offset = 0;
    uint64_t size         = 16384;
    ww_evt_bind_buffers_t bind         = {
        .buffer_generation = buffer_generation,
        .count             = 1,
        .width             = 64,
        .height            = 64,
        .fourcc            = 0x34324241,
        .modifier          = 0,
        .planes_per_buffer = 1,
        .stride            = { .count = 1, .data = &stride },
        .plane_offset      = { .count = 1, .data = &plane_offset },
        .size              = { .count = 1, .data = &size },
        .initial_config = {
            .generation = composition_generation,
            .buffer_generation = buffer_generation,
            .source_rect = { .x = 0, .y = 0, .w = 64, .h = 64 },
            .dest_rect = { .x = 0, .y = 0, .w = 64, .h = 64 },
            .transform = 0,
            .clear_color = { .r = 0.1f, .g = 0.2f, .b = 0.3f, .a = 1.0f },
        },
    };
    ww_buf_t out;
    ww_buf_init(&out);
    int rc = ww_evt_bind_buffers_encode(&bind, &out);
    if (rc == WW_OK) {
        rc = ww_codec_send_event(client_fd, WW_EVT_BIND_BUFFERS, out.data, out.len, dmabuf, 1);
    }
    ww_buf_free(&out);
    close(dmabuf[0]);
    close(dmabuf[1]);
    return rc;
}

static int send_composition_config(int client_fd, uint64_t buffer_generation,
                                   uint64_t composition_generation) {
    ww_evt_set_composition_config_t event = {
        .config = {
            .generation = composition_generation,
            .buffer_generation = buffer_generation,
            .source_rect = { .x = 0, .y = 0, .w = 64, .h = 64 },
            .dest_rect = { .x = 0, .y = 0, .w = 64, .h = 64 },
            .transform = 0,
            .clear_color = { .r = 0.1f, .g = 0.2f, .b = 0.3f, .a = 1.0f },
        },
    };
    ww_buf_t out;
    ww_buf_init(&out);
    int rc = ww_evt_set_composition_config_encode(&event, &out);
    if (rc == WW_OK) {
        rc = ww_codec_send_event(
            client_fd, WW_EVT_SET_COMPOSITION_CONFIG, out.data, out.len, NULL, 0);
    }
    ww_buf_free(&out);
    return rc;
}

static int send_presentation_snapshot(int client_fd, uint64_t config_generation,
                                      uint64_t state_generation, waywallen_pause_effect_kind_t kind,
                                      uint32_t radius, bool active) {
    ww_evt_set_presentation_snapshot_t event           = { 0 };
    event.presentation.config.generation               = config_generation;
    event.presentation.config.pause_effect.kind        = kind;
    event.presentation.config.pause_effect.blur.radius = radius;
    event.presentation.state.generation                = state_generation;
    event.presentation.state.config_generation         = config_generation;
    event.presentation.state.pause_effect.active       = active;
    ww_buf_t out;
    ww_buf_init(&out);
    int rc = ww_evt_set_presentation_snapshot_encode(&event, &out);
    if (rc == WW_OK) {
        rc = ww_codec_send_event(
            client_fd, WW_EVT_SET_PRESENTATION_SNAPSHOT, out.data, out.len, NULL, 0);
    }
    ww_buf_free(&out);
    return rc;
}

static int send_presentation_state(int client_fd, uint64_t state_generation,
                                   uint64_t config_generation, bool active) {
    ww_evt_set_presentation_state_t event = { 0 };
    event.state.generation                = state_generation;
    event.state.config_generation         = config_generation;
    event.state.pause_effect.active       = active;
    ww_buf_t out;
    ww_buf_init(&out);
    int rc = ww_evt_set_presentation_state_encode(&event, &out);
    if (rc == WW_OK) {
        rc = ww_codec_send_event(
            client_fd, WW_EVT_SET_PRESENTATION_STATE, out.data, out.len, NULL, 0);
    }
    ww_buf_free(&out);
    return rc;
}

static int send_frame_ready(int client_fd, uint64_t buffer_generation, uint64_t seq) {
    int acquire[2];
    int release[2];
    if (pipe(acquire) != 0) return -1;
    if (pipe(release) != 0) {
        close(acquire[0]);
        close(acquire[1]);
        return -1;
    }
    ww_evt_frame_ready_t frame = {
        .buffer_generation = buffer_generation,
        .buffer_index      = 0,
        .seq               = seq,
    };
    ww_buf_t out;
    ww_buf_init(&out);
    int rc = ww_evt_frame_ready_encode(&frame, &out);
    if (rc == WW_OK) {
        int fds[2] = { acquire[0], release[0] };
        rc         = ww_codec_send_event(client_fd, WW_EVT_FRAME_READY, out.data, out.len, fds, 2);
    }
    ww_buf_free(&out);
    close(acquire[0]);
    close(acquire[1]);
    close(release[0]);
    close(release[1]);
    return rc;
}

static int send_unbind(int client_fd, uint64_t buffer_generation) {
    ww_evt_unbind_t unbind = { .buffer_generation = buffer_generation };
    ww_buf_t        out;
    ww_buf_init(&out);
    int rc = ww_evt_unbind_encode(&unbind, &out);
    if (rc == WW_OK) {
        rc = ww_codec_send_event(client_fd, WW_EVT_UNBIND, out.data, out.len, NULL, 0);
    }
    ww_buf_free(&out);
    return rc;
}

static int handler_generation_sequence(int client_fd, struct test_state* ts) {
    if (complete_handshake_capture_caps(client_fd, ts) != 0) return -1;
    if (send_bind_buffers(client_fd, 7, 11) != 0) return -1;
    if (send_frame_ready(client_fd, 7, 2) != 0) return -1;
    sleep_ms(50);
    return 0;
}

static int handler_stale_frame_invalid_release(int client_fd, struct test_state* ts) {
    if (complete_handshake_capture_caps(client_fd, ts) != 0) return -1;
    if (send_bind_buffers(client_fd, 7, 1) != 0) return -1;
    if (send_frame_ready(client_fd, 6, 1) != 0) return -1;
    sleep_ms(50);
    return 0;
}

static int handler_composition_update(int client_fd, struct test_state* ts) {
    if (complete_handshake_capture_caps(client_fd, ts) != 0) return -1;
    if (send_bind_buffers(client_fd, 9, 1) != 0) return -1;
    if (send_composition_config(client_fd, 9, 2) != 0) return -1;
    sleep_ms(50);
    return 0;
}

static int handler_non_monotonic_bind(int client_fd, struct test_state* ts) {
    if (complete_handshake_capture_caps(client_fd, ts) != 0) return -1;
    if (send_bind_buffers(client_fd, 7, 1) != 0) return -1;
    if (send_bind_buffers(client_fd, 7, 2) != 0) return -1;
    sleep_ms(50);
    return 0;
}

static int handler_mismatched_unbind(int client_fd, struct test_state* ts) {
    if (complete_handshake_capture_caps(client_fd, ts) != 0) return -1;
    if (send_bind_buffers(client_fd, 7, 1) != 0) return -1;
    if (send_unbind(client_fd, 8) != 0) return -1;
    sleep_ms(50);
    return 0;
}

static int handler_set_composition_while_idle(int client_fd, struct test_state* ts) {
    if (complete_handshake_capture_caps(client_fd, ts) != 0) return -1;
    if (send_composition_config(client_fd, 1, 1) != 0) return -1;
    sleep_ms(50);
    return 0;
}

static int handler_unbind_pool_one(int client_fd, struct test_state* ts) {
    if (complete_handshake_capture_caps(client_fd, ts) != 0) return -1;
    if (send_bind_buffers(client_fd, 7, 1) != 0) return -1;
    if (send_unbind(client_fd, 7) != 0) return -1;
    return receive_ack_unbind(client_fd, ts);
}

static int handler_unbind_pool_two(int client_fd, struct test_state* ts) {
    if (complete_handshake_capture_caps(client_fd, ts) != 0) return -1;
    if (send_bind_buffers(client_fd, 7, 1) != 0) return -1;
    if (send_unbind(client_fd, 7) != 0) return -1;
    return receive_ack_unbind(client_fd, ts);
}

static int handler_import_failure(int client_fd, struct test_state* ts) {
    if (complete_handshake_capture_caps(client_fd, ts) != 0) return -1;
    if (send_bind_buffers(client_fd, 7, 1) != 0) return -1;
    return receive_import_failure(client_fd, ts);
}

static int handler_outbox_semantics(int client_fd, struct test_state* ts) {
    if (complete_handshake_capture_caps(client_fd, ts) != 0) return -1;
    sleep_ms(100);

    static uint8_t body_buf[WW_CODEC_MAX_BODY_BYTES];
    uint32_t       requests_seen = 0;
    for (;;) {
        uint16_t op;
        size_t   body_len;
        int      fds[4];
        size_t   n_fds;
        int      rc = ww_codec_recv_request(
            client_fd, &op, body_buf, WW_CODEC_MAX_BODY_BYTES, &body_len, fds, 4, &n_fds);
        if (rc != 0) return 0;
        switch (op) {
        case WW_REQ_POINTER_AXIS: {
            ww_req_pointer_axis_t request;
            if (ww_req_pointer_axis_decode(body_buf, body_len, &request) != WW_OK) return -1;
            ts->outbox_axis_count++;
            ww_req_pointer_axis_free(&request);
            break;
        }
        case WW_REQ_POINTER_MOTION: {
            ww_req_pointer_motion_t request;
            if (ww_req_pointer_motion_decode(body_buf, body_len, &request) != WW_OK) return -1;
            ts->outbox_motion_count++;
            ts->outbox_last_motion_x = request.x;
            ww_req_pointer_motion_free(&request);
            break;
        }
        case WW_REQ_SET_DISPLAY_METRICS: {
            ww_req_set_display_metrics_t request;
            if (ww_req_set_display_metrics_decode(body_buf, body_len, &request) != WW_OK) return -1;
            ts->outbox_metrics_count++;
            ts->outbox_last_metrics_width = request.metrics.width;
            ww_req_set_display_metrics_free(&request);
            break;
        }
        case WW_REQ_SET_WINDOW_STATE: {
            ww_req_set_window_state_t request;
            if (ww_req_set_window_state_decode(body_buf, body_len, &request) != WW_OK) return -1;
            ts->outbox_window_count++;
            ts->outbox_last_window_flags = request.flags;
            ww_req_set_window_state_free(&request);
            break;
        }
        case WW_REQ_FRAME_RELEASE_ARMED: {
            ww_req_frame_release_armed_t request;
            if (ww_req_frame_release_armed_decode(body_buf, body_len, &request) != WW_OK) return -1;
            ts->outbox_saw_critical             = 1;
            ts->outbox_requests_before_critical = requests_seen;
            ww_req_frame_release_armed_free(&request);
            break;
        }
        default: return -1;
        }
        requests_seen++;
    }
}

static int handler_bind_generation_one(int client_fd, struct test_state* ts) {
    if (complete_handshake_capture_caps(client_fd, ts) != 0) return -1;
    if (send_bind_buffers(client_fd, 1, 1) != 0) return -1;
    sleep_ms(50);
    return 0;
}

static int handler_presentation_updates(int client_fd, struct test_state* ts) {
    if (complete_handshake_capture_caps(client_fd, ts) != 0) return -1;
    if (send_presentation_snapshot(client_fd, 2, 2, WAYWALLEN_PAUSE_EFFECT_KIND_BLUR, 40, true) !=
        0)
        return -1;
    if (send_presentation_state(client_fd, 3, 2, false) != 0) return -1;
    sleep_ms(50);
    return 0;
}

static int handler_cross_generation_state(int client_fd, struct test_state* ts) {
    if (complete_handshake_capture_caps(client_fd, ts) != 0) return -1;
    if (send_presentation_state(client_fd, 2, 99, false) != 0) return -1;
    sleep_ms(50);
    return 0;
}

static int handler_invalid_presentation_radius(int client_fd, struct test_state* ts) {
    if (complete_handshake_capture_caps(client_fd, ts) != 0) return -1;
    if (send_presentation_snapshot(client_fd, 2, 2, WAYWALLEN_PAUSE_EFFECT_KIND_BLUR, 65, true) !=
        0)
        return -1;
    sleep_ms(50);
    return 0;
}

static int handler_unknown_pause_effect_kind(int client_fd, struct test_state* ts) {
    if (complete_handshake_capture_caps(client_fd, ts) != 0) return -1;
    if (send_presentation_snapshot(client_fd, 2, 2, (waywallen_pause_effect_kind_t)7, 30, false) !=
        0)
        return -1;
    sleep_ms(50);
    return 0;
}

/* Recv hello then send welcome in two writes (header bytes 0..1, then
 * 2..3+body) with a small pause. Forces the client's recv state machine
 * to handle a partial header. */
static int handler_partial_welcome(int client_fd, struct test_state* ts) {
    (void)ts;
    static uint8_t body_buf[WW_CODEC_MAX_BODY_BYTES];
    uint16_t       op;
    size_t         body_len;
    int            fds[4];
    size_t         n_fds;

    int rc = ww_codec_recv_request(
        client_fd, &op, body_buf, WW_CODEC_MAX_BODY_BYTES, &body_len, fds, 4, &n_fds);
    if (rc != 0 || op != WW_REQ_HELLO) return -1;

    /* Encode welcome body. */
    ww_evt_welcome_t welcome;
    memset(&welcome, 0, sizeof(welcome));
    welcome.server_version = (char*)"mock-server/0.1";
    ww_buf_t body;
    ww_buf_init(&body);
    if (ww_evt_welcome_encode(&welcome, &body) != WW_OK) {
        ww_buf_free(&body);
        return -1;
    }

    /* Manual framing — split header across two writes with a sleep
     * between them so the kernel surfaces an EAGAIN to the client's
     * non-blocking recv between bytes 2 and 3. */
    size_t  total = 4 + body.len;
    uint8_t hdr[4];
    hdr[0] = (uint8_t)(WW_EVT_WELCOME & 0xff);
    hdr[1] = (uint8_t)((WW_EVT_WELCOME >> 8) & 0xff);
    hdr[2] = (uint8_t)(total & 0xff);
    hdr[3] = (uint8_t)((total >> 8) & 0xff);

    ssize_t w = write(client_fd, hdr, 2);
    if (w != 2) {
        ww_buf_free(&body);
        return -1;
    }
    sleep_ms(20);
    w = write(client_fd, hdr + 2, 2);
    if (w != 2) {
        ww_buf_free(&body);
        return -1;
    }
    sleep_ms(20);
    if (body.len > 0) {
        w = write(client_fd, body.data, body.len);
        if ((size_t)w != body.len) {
            ww_buf_free(&body);
            return -1;
        }
    }
    ww_buf_free(&body);

    /* Continue handshake normally. */
    rc = ww_codec_recv_request(
        client_fd, &op, body_buf, WW_CODEC_MAX_BODY_BYTES, &body_len, fds, 4, &n_fds);
    if (rc != 0 || op != WW_REQ_REGISTER_DISPLAY) return -1;
    ww_evt_display_accepted_t accepted = accepted_event(7);
    ww_buf_t                  out;
    ww_buf_init(&out);
    if (ww_evt_display_accepted_encode(&accepted, &out) != WW_OK) {
        ww_buf_free(&out);
        return -1;
    }
    rc = ww_codec_send_event(client_fd, WW_EVT_DISPLAY_ACCEPTED, out.data, out.len, NULL, 0);
    ww_buf_free(&out);
    return rc;
}

/* Recv hello then close immediately. The client should observe ECONNRESET
 * during the welcome wait and surface on_disconnected. */
static int handler_close_after_hello(int client_fd, struct test_state* ts) {
    (void)ts;
    static uint8_t body_buf[WW_CODEC_MAX_BODY_BYTES];
    uint16_t       op;
    size_t         body_len;
    int            fds[4];
    size_t         n_fds;
    int            rc = ww_codec_recv_request(
        client_fd, &op, body_buf, WW_CODEC_MAX_BODY_BYTES, &body_len, fds, 4, &n_fds);
    (void)rc;
    return 0; /* return -> close(client_fd) in the thread shim */
}

/* Recv hello then send WW_EVT_ERROR (XML op=7) instead of welcome. */
static int handler_send_error_after_hello(int client_fd, struct test_state* ts) {
    (void)ts;
    static uint8_t body_buf[WW_CODEC_MAX_BODY_BYTES];
    uint16_t       op;
    size_t         body_len;
    int            fds[4];
    size_t         n_fds;
    int            rc = ww_codec_recv_request(
        client_fd, &op, body_buf, WW_CODEC_MAX_BODY_BYTES, &body_len, fds, 4, &n_fds);
    if (rc != 0 || op != WW_REQ_HELLO) return -1;

    ww_evt_error_t er;
    er.code    = WAYWALLEN_DISPLAY_ERROR_CODE_INTERNAL;
    er.message = (char*)"nope";
    ww_buf_t out;
    ww_buf_init(&out);
    if (ww_evt_error_encode(&er, &out) != WW_OK) {
        ww_buf_free(&out);
        return -1;
    }
    rc = ww_codec_send_event(client_fd, WW_EVT_ERROR, out.data, out.len, NULL, 0);
    ww_buf_free(&out);
    return rc;
}

/* ------------------------------------------------------------------ */
/*  Tests                                                              */
/* ------------------------------------------------------------------ */

static void test_connect_to_nonexistent_socket(void) {
    /* No listener at all — begin_connect should fail fast with an IO
     * error and never block. */
    struct test_state ts;
    memset(&ts, 0, sizeof(ts));
    snprintf(
        ts.sock_path, sizeof(ts.sock_path), "/tmp/waywallen-nonexistent-%d.sock", (int)getpid());
    unlink(ts.sock_path);

    waywallen_display_t* d  = make_client(&ts);
    int                  rc = begin_test_display(d, ts.sock_path, 1920, 1080);
    assert(rc != WAYWALLEN_OK);
    waywallen_display_free(d);
    printf("  ok test_connect_to_nonexistent_socket (rc=%d)\n", rc);
}

static void test_legacy_blocking_connect(void) {
    struct test_state ts;
    ts_init(&ts);
    pthread_t srv = spawn_server(&ts, handler_full_handshake);

    waywallen_display_t* d  = make_client(&ts);
    int                  rc = connect_test_display(d, ts.sock_path, 1920, 1080);
    assert(rc == WAYWALLEN_OK);
    assert(ts.on_disconnected_count == 0);
    waywallen_display_close(d);
    waywallen_display_free(d);

    pthread_join(srv, NULL);
    ts_teardown(&ts);
    printf("  ok test_legacy_blocking_connect\n");
}

static void test_begin_connect_immediate(void) {
    struct test_state ts;
    ts_init(&ts);
    pthread_t srv = spawn_server(&ts, handler_full_handshake);

    waywallen_display_t* d  = make_client(&ts);
    int                  rc = begin_test_display(d, ts.sock_path, 640, 480);
    assert(rc == WAYWALLEN_OK);
    assert(waywallen_display_get_fd(d) >= 0);
    /* On a sync accept the kernel completes connect immediately, so the
     * state machine is at HELLO_PENDING (hello queued, partial sender
     * about to drain). On a slow accept it can be CONNECTING. Either is
     * legal; both are non-IDLE / non-READY. */
    waywallen_handshake_state_t hs = waywallen_display_handshake_state(d);
    assert(hs == WAYWALLEN_HS_HELLO_PENDING || hs == WAYWALLEN_HS_CONNECTING);

    /* Finish the handshake so the server thread doesn't dangle. */
    rc = drive_handshake(d, 1000);
    assert(rc == WAYWALLEN_OK);

    waywallen_display_close(d);
    waywallen_display_free(d);
    pthread_join(srv, NULL);
    ts_teardown(&ts);
    printf("  ok test_begin_connect_immediate\n");
}

static void test_full_handshake_via_async_api(void) {
    struct test_state ts;
    ts_init(&ts);
    pthread_t srv = spawn_server(&ts, handler_full_handshake);

    waywallen_display_t* d = make_client(&ts);
    assert(waywallen_display_set_presentation_caps(d, 1u << 8) == WAYWALLEN_ERR_INVAL);
    assert(waywallen_display_set_presentation_caps(d, WAYWALLEN_PRESENTATION_CAP_PAUSE_BLUR) ==
           WAYWALLEN_OK);
    assert(waywallen_display_set_window_state(d, 1u << 8) == WAYWALLEN_ERR_INVAL);
    assert(waywallen_display_set_window_state(d, WAYWALLEN_WIN_HAS_ACTIVE) == WAYWALLEN_OK);
    int rc = begin_test_display(d, ts.sock_path, 1920, 1080);
    assert(rc == WAYWALLEN_OK);
    assert(waywallen_display_set_presentation_caps(d, 0) == WAYWALLEN_ERR_STATE);
    rc = drive_handshake(d, 2000);
    assert(rc == WAYWALLEN_OK);
    assert(waywallen_display_handshake_state(d) == WAYWALLEN_HS_READY);
    assert(waywallen_display_conn_state(d) == WAYWALLEN_CONN_CONNECTED);
    assert(ts.on_disconnected_count == 0);
    assert(ts.presentation_caps == WAYWALLEN_PRESENTATION_CAP_PAUSE_BLUR);
    assert(ts.registered_window_state == WAYWALLEN_WIN_HAS_ACTIVE);
    assert(ts.registered_metrics.width == 1920);
    assert(ts.registered_metrics.height == 1080);
    assert(ts.registered_metrics.refresh_mhz == 60000);
    assert(ts.on_presentation_snapshot_count == 1);
    waywallen_presentation_snapshot_t presentation;
    assert(waywallen_display_get_presentation_snapshot(d, &presentation) == WAYWALLEN_OK);
    assert(presentation.config.generation == 1);
    assert(presentation.config.pause_effect.kind == WAYWALLEN_PAUSE_EFFECT_KIND_NONE);

    const waywallen_display_metrics_t unknown_refresh = { .width       = 1920,
                                                          .height      = 1080,
                                                          .refresh_mhz = 0 };
    assert(waywallen_display_set_metrics(d, &unknown_refresh) == WAYWALLEN_OK);
    assert(waywallen_display_send_pointer_motion(d, 1.0f, 2.0f, 3, 1u << 4) == WAYWALLEN_ERR_INVAL);
    assert(waywallen_display_send_pointer_button(
               d, 1.0f, 2.0f, 272, (waywallen_pointer_button_state_t)9, 3, 0) ==
           WAYWALLEN_ERR_INVAL);
    assert(waywallen_display_send_pointer_axis(
               d, 1.0f, 2.0f, 0.0f, 1.0f, (waywallen_pointer_axis_source_t)9, 3, 0) ==
           WAYWALLEN_ERR_INVAL);

    waywallen_display_close(d);
    waywallen_display_free(d);
    pthread_join(srv, NULL);
    assert(ts.registered_metrics.refresh_mhz == 0);
    ts_teardown(&ts);
    printf("  ok test_full_handshake_via_async_api\n");
}

static void test_presentation_snapshot_updates_and_disconnect_reset(void) {
    struct test_state ts;
    ts_init(&ts);
    pthread_t srv = spawn_server(&ts, handler_presentation_updates);

    waywallen_display_t* d = make_client(&ts);
    assert(begin_test_display(d, ts.sock_path, 1920, 1080) == WAYWALLEN_OK);
    assert(drive_handshake(d, 2000) == WAYWALLEN_OK);
    assert(ts.on_presentation_snapshot_count == 1);

    assert(dispatch_next_event(d) == WAYWALLEN_OK);
    assert(ts.on_presentation_snapshot_count == 2);
    assert(ts.last_presentation.config.generation == 2);
    assert(ts.last_presentation.config.pause_effect.kind == WAYWALLEN_PAUSE_EFFECT_KIND_BLUR);
    assert(ts.last_presentation.config.pause_effect.blur.radius == 40);
    assert(ts.last_presentation.state.pause_effect.active);

    assert(dispatch_next_event(d) == WAYWALLEN_OK);
    assert(ts.on_presentation_state_count == 1);
    assert(ts.last_presentation.state.generation == 3);
    assert(! ts.last_presentation.state.pause_effect.active);

    pthread_join(srv, NULL);
    assert(dispatch_next_event(d) == WAYWALLEN_ERR_NOTCONN);
    assert(ts.on_presentation_snapshot_count == 3);
    assert(ts.last_presentation.config.pause_effect.kind == WAYWALLEN_PAUSE_EFFECT_KIND_NONE);
    assert(! ts.last_presentation.state.pause_effect.active);
    assert(waywallen_display_get_presentation_snapshot(d, &ts.last_presentation) ==
           WAYWALLEN_ERR_NOTCONN);

    waywallen_display_free(d);
    ts_teardown(&ts);
    printf("  ok test_presentation_snapshot_updates_and_disconnect_reset\n");
}

static void test_cross_generation_state_is_protocol_error(void) {
    struct test_state ts;
    ts_init(&ts);
    pthread_t srv = spawn_server(&ts, handler_cross_generation_state);

    waywallen_display_t* d = make_client(&ts);
    assert(begin_test_display(d, ts.sock_path, 1920, 1080) == WAYWALLEN_OK);
    assert(drive_handshake(d, 2000) == WAYWALLEN_OK);
    assert(dispatch_next_event(d) == WAYWALLEN_ERR_PROTO);
    assert(ts.on_presentation_state_count == 0);
    assert(ts.on_disconnected_count == 1);
    assert(strcmp(ts.last_disconnect_msg, "invalid presentation state") == 0);

    waywallen_display_free(d);
    pthread_join(srv, NULL);
    ts_teardown(&ts);
    printf("  ok test_cross_generation_state_is_protocol_error\n");
}

static void test_invalid_presentation_radius_is_protocol_error(void) {
    struct test_state ts;
    ts_init(&ts);
    pthread_t srv = spawn_server(&ts, handler_invalid_presentation_radius);

    waywallen_display_t* d = make_client(&ts);
    assert(begin_test_display(d, ts.sock_path, 1920, 1080) == WAYWALLEN_OK);
    assert(drive_handshake(d, 2000) == WAYWALLEN_OK);
    assert(dispatch_next_event(d) == WAYWALLEN_ERR_PROTO);
    assert(ts.on_presentation_snapshot_count == 2);
    assert(ts.last_presentation.config.pause_effect.kind == WAYWALLEN_PAUSE_EFFECT_KIND_NONE);
    assert(ts.on_disconnected_count == 1);
    assert(strcmp(ts.last_disconnect_msg, "invalid presentation snapshot") == 0);

    waywallen_display_free(d);
    pthread_join(srv, NULL);
    ts_teardown(&ts);
    printf("  ok test_invalid_presentation_radius_is_protocol_error\n");
}

static void test_unknown_pause_effect_kind_is_protocol_error(void) {
    struct test_state ts;
    ts_init(&ts);
    pthread_t srv = spawn_server(&ts, handler_unknown_pause_effect_kind);

    waywallen_display_t* d = make_client(&ts);
    assert(begin_test_display(d, ts.sock_path, 1920, 1080) == WAYWALLEN_OK);
    assert(drive_handshake(d, 2000) == WAYWALLEN_OK);
    assert(dispatch_next_event(d) == WAYWALLEN_ERR_PROTO);
    assert(ts.on_disconnected_count == 1);
    assert(strcmp(ts.last_disconnect_msg, "decode set_presentation_snapshot") == 0);

    waywallen_display_free(d);
    pthread_join(srv, NULL);
    ts_teardown(&ts);
    printf("  ok test_unknown_pause_effect_kind_is_protocol_error\n");
}

/* No backend bound → consumer_caps probe falls through to the
 * hardcoded ABGR/XRGB + LINEAR fallback. The library should:
 *   - set HOST_VISIBLE in mem_hints (always)
 *   - set LINEAR_ONLY in mem_hints (every advertised modifier is LINEAR)
 *   - leave DEVICE_LOCAL clear (no Vulkan probe ran)
 *   - advertise both BINARY+TIMELINE in sync_caps unconditionally */
static void test_consumer_caps_signals_linear_only_when_no_backend(void) {
    struct test_state ts;
    ts_init(&ts);
    pthread_t srv = spawn_server(&ts, handler_full_handshake_capture_caps);

    waywallen_display_t* d  = make_client(&ts);
    int                  rc = begin_test_display(d, ts.sock_path, 1920, 1080);
    assert(rc == WAYWALLEN_OK);
    rc = drive_handshake(d, 2000);
    assert(rc == WAYWALLEN_OK);

    pthread_join(srv, NULL);

    assert(ts.saw_consumer_caps && "server never received consumer_caps");

    const uint32_t WW_MEM_HINT_DEVICE_LOCAL = 1u << 0;
    const uint32_t WW_MEM_HINT_HOST_VISIBLE = 1u << 1;
    const uint32_t WW_MEM_HINT_LINEAR_ONLY  = 1u << 4;
    const uint32_t WW_SYNC_SYNCOBJ_BINARY   = 1u << 1;
    const uint32_t WW_SYNC_SYNCOBJ_TIMELINE = 1u << 2;

    assert((ts.consumer_caps_mem_hints & WW_MEM_HINT_HOST_VISIBLE) != 0 &&
           "expected HOST_VISIBLE in mem_hints");
    assert((ts.consumer_caps_mem_hints & WW_MEM_HINT_LINEAR_ONLY) != 0 &&
           "expected LINEAR_ONLY in mem_hints (no backend → fallback)");
    assert((ts.consumer_caps_mem_hints & WW_MEM_HINT_DEVICE_LOCAL) == 0 &&
           "DEVICE_LOCAL must not be advertised without Vulkan probe");
    assert((ts.consumer_caps_sync_caps & (WW_SYNC_SYNCOBJ_BINARY | WW_SYNC_SYNCOBJ_TIMELINE)) ==
               (WW_SYNC_SYNCOBJ_BINARY | WW_SYNC_SYNCOBJ_TIMELINE) &&
           "sync_caps must advertise BINARY+TIMELINE");

    waywallen_display_close(d);
    waywallen_display_free(d);
    ts_teardown(&ts);
    printf("  ok test_consumer_caps_signals_linear_only_when_no_backend "
           "(mem_hints=0x%x sync_caps=0x%x)\n",
           ts.consumer_caps_mem_hints,
           ts.consumer_caps_sync_caps);
}

static void test_partial_welcome(void) {
    struct test_state ts;
    ts_init(&ts);
    pthread_t srv = spawn_server(&ts, handler_partial_welcome);

    waywallen_display_t* d  = make_client(&ts);
    int                  rc = begin_test_display(d, ts.sock_path, 1920, 1080);
    assert(rc == WAYWALLEN_OK);
    rc = drive_handshake(d, 2000);
    assert(rc == WAYWALLEN_OK);
    assert(ts.on_disconnected_count == 0);

    waywallen_display_close(d);
    waywallen_display_free(d);
    pthread_join(srv, NULL);
    ts_teardown(&ts);
    printf("  ok test_partial_welcome\n");
}

static void test_server_closes_during_welcome_wait(void) {
    struct test_state ts;
    ts_init(&ts);
    pthread_t srv = spawn_server(&ts, handler_close_after_hello);

    waywallen_display_t* d  = make_client(&ts);
    int                  rc = begin_test_display(d, ts.sock_path, 1920, 1080);
    assert(rc == WAYWALLEN_OK);
    rc = drive_handshake(d, 1000);
    assert(rc == WAYWALLEN_ERR_NOTCONN);
    assert(ts.on_disconnected_count == 1);

    waywallen_display_free(d);
    pthread_join(srv, NULL);
    ts_teardown(&ts);
    printf("  ok test_server_closes_during_welcome_wait\n");
}

static void test_server_sends_error_event(void) {
    struct test_state ts;
    ts_init(&ts);
    pthread_t srv = spawn_server(&ts, handler_send_error_after_hello);

    waywallen_display_t* d  = make_client(&ts);
    int                  rc = begin_test_display(d, ts.sock_path, 1920, 1080);
    assert(rc == WAYWALLEN_OK);
    rc = drive_handshake(d, 1000);
    assert(rc == WAYWALLEN_ERR_PROTO);
    assert(ts.on_disconnected_count == 1);
    assert(strcmp(ts.last_disconnect_msg, "nope") == 0);

    waywallen_display_free(d);
    pthread_join(srv, NULL);
    ts_teardown(&ts);
    printf("  ok test_server_sends_error_event\n");
}

static int dispatch_next_event(waywallen_display_t* d) {
    struct pollfd pfd = {
        .fd      = waywallen_display_get_fd(d),
        .events  = POLLIN,
        .revents = 0,
    };
    int rc = poll(&pfd, 1, 2000);
    assert(rc == 1);
    return waywallen_display_dispatch(d);
}

static void test_atomic_binding_payload(void) {
    struct test_state ts;
    ts_init(&ts);
    pthread_t srv = spawn_server(&ts, handler_generation_sequence);

    waywallen_display_t* d  = make_client(&ts);
    int                  rc = begin_test_display(d, ts.sock_path, 1920, 1080);
    assert(rc == WAYWALLEN_OK);
    assert(drive_handshake(d, 2000) == WAYWALLEN_OK);

    assert(dispatch_next_event(d) == WAYWALLEN_OK); /* bind gen=7 */
    assert(ts.on_binding_ready_count == 1);
    assert(ts.last_binding_buffer_generation == 7);
    assert(ts.last_composition_buffer_generation == 7);
    assert(ts.last_composition_generation == 11);
    assert(ts.on_composition_config_count == 0);

    assert(dispatch_next_event(d) == WAYWALLEN_OK); /* frame gen=7 */
    assert(ts.on_frame_ready_count == 1);
    assert(ts.last_frame_buffer_generation == 7);
    assert(ts.on_disconnected_count == 0);

    waywallen_display_close(d);
    waywallen_display_free(d);
    pthread_join(srv, NULL);
    ts_teardown(&ts);
    printf("  ok test_atomic_binding_payload\n");
}

static void test_invalid_stale_release_disconnects(void) {
    struct test_state ts;
    ts_init(&ts);
    pthread_t srv = spawn_server(&ts, handler_stale_frame_invalid_release);

    waywallen_display_t* d  = make_client(&ts);
    int                  rc = begin_test_display(d, ts.sock_path, 1920, 1080);
    assert(rc == WAYWALLEN_OK);
    assert(drive_handshake(d, 2000) == WAYWALLEN_OK);
    assert(dispatch_next_event(d) == WAYWALLEN_OK);
    assert(dispatch_next_event(d) == WAYWALLEN_ERR_IO);
    assert(ts.on_frame_ready_count == 0);
    assert(ts.on_disconnected_count == 1);
    assert(strcmp(ts.last_disconnect_msg, "stale frame release failed") == 0);

    waywallen_display_free(d);
    pthread_join(srv, NULL);
    ts_teardown(&ts);
    printf("  ok test_invalid_stale_release_disconnects\n");
}

static void test_composition_update_targets_current_binding(void) {
    struct test_state ts;
    ts_init(&ts);
    pthread_t srv = spawn_server(&ts, handler_composition_update);

    waywallen_display_t* d  = make_client(&ts);
    int                  rc = begin_test_display(d, ts.sock_path, 1920, 1080);
    assert(rc == WAYWALLEN_OK);
    assert(drive_handshake(d, 2000) == WAYWALLEN_OK);

    assert(dispatch_next_event(d) == WAYWALLEN_OK);
    assert(ts.on_binding_ready_count == 1);
    assert(dispatch_next_event(d) == WAYWALLEN_OK);
    assert(ts.on_composition_config_count == 1);
    assert(ts.last_composition_generation == 2);
    assert(ts.last_composition_buffer_generation == 9);
    assert(ts.on_disconnected_count == 0);

    waywallen_display_free(d);
    pthread_join(srv, NULL);
    ts_teardown(&ts);
    printf("  ok test_composition_update_targets_current_binding\n");
}

static void test_non_monotonic_buffer_generation_is_protocol_error(void) {
    struct test_state ts;
    ts_init(&ts);
    pthread_t srv = spawn_server(&ts, handler_non_monotonic_bind);

    waywallen_display_t* d  = make_client(&ts);
    int                  rc = begin_test_display(d, ts.sock_path, 1920, 1080);
    assert(rc == WAYWALLEN_OK);
    assert(drive_handshake(d, 2000) == WAYWALLEN_OK);

    assert(dispatch_next_event(d) == WAYWALLEN_OK);
    assert(ts.on_binding_ready_count == 1);
    assert(dispatch_next_event(d) == WAYWALLEN_ERR_PROTO);
    assert(ts.on_binding_ready_count == 1);
    assert(ts.on_disconnected_count == 1);
    assert(strcmp(ts.last_disconnect_msg, "non-monotonic buffer_generation") == 0);

    waywallen_display_free(d);
    pthread_join(srv, NULL);
    ts_teardown(&ts);
    printf("  ok test_non_monotonic_buffer_generation_is_protocol_error\n");
}

static void test_mismatched_unbind_generation_is_protocol_error(void) {
    struct test_state ts;
    ts_init(&ts);
    pthread_t srv = spawn_server(&ts, handler_mismatched_unbind);

    waywallen_display_t* d  = make_client(&ts);
    int                  rc = begin_test_display(d, ts.sock_path, 1920, 1080);
    assert(rc == WAYWALLEN_OK);
    assert(drive_handshake(d, 2000) == WAYWALLEN_OK);

    assert(dispatch_next_event(d) == WAYWALLEN_OK);
    assert(ts.on_binding_ready_count == 1);
    assert(dispatch_next_event(d) == WAYWALLEN_ERR_PROTO);
    assert(ts.on_textures_releasing_count == 0);
    assert(ts.on_disconnected_count == 1);
    assert(strcmp(ts.last_disconnect_msg, "unbind generation mismatch") == 0);

    waywallen_display_free(d);
    pthread_join(srv, NULL);
    ts_teardown(&ts);
    printf("  ok test_mismatched_unbind_generation_is_protocol_error\n");
}

static void test_set_composition_while_idle_is_protocol_error(void) {
    struct test_state ts;
    ts_init(&ts);
    pthread_t srv = spawn_server(&ts, handler_set_composition_while_idle);

    waywallen_display_t* d  = make_client(&ts);
    int                  rc = begin_test_display(d, ts.sock_path, 1920, 1080);
    assert(rc == WAYWALLEN_OK);
    assert(drive_handshake(d, 2000) == WAYWALLEN_OK);

    assert(dispatch_next_event(d) == WAYWALLEN_ERR_PROTO);
    assert(ts.on_composition_config_count == 0);
    assert(ts.on_disconnected_count == 1);
    assert(strcmp(ts.last_disconnect_msg, "invalid composition config") == 0);

    waywallen_display_free(d);
    pthread_join(srv, NULL);
    ts_teardown(&ts);
    printf("  ok test_set_composition_while_idle_is_protocol_error\n");
}

static void test_unbind_is_valid_for_atomic_binding(void) {
    int (*server_handlers[])(int, struct test_state*) = { handler_unbind_pool_one,
                                                          handler_unbind_pool_two };
    for (size_t i = 0; i < sizeof(server_handlers) / sizeof(server_handlers[0]); ++i) {
        struct test_state ts;
        ts_init(&ts);
        pthread_t srv = spawn_server(&ts, server_handlers[i]);

        waywallen_display_t* d  = make_client(&ts);
        int                  rc = begin_test_display(d, ts.sock_path, 1920, 1080);
        assert(rc == WAYWALLEN_OK);
        assert(drive_handshake(d, 2000) == WAYWALLEN_OK);

        assert(dispatch_next_event(d) == WAYWALLEN_OK);
        assert(dispatch_next_event(d) == WAYWALLEN_OK);
        assert(ts.on_textures_releasing_count == 1);
        assert(ts.on_disconnected_count == 0);
        assert(waywallen_display_stream_state(d) == WAYWALLEN_STREAM_INACTIVE);

        waywallen_display_close(d);
        waywallen_display_free(d);
        pthread_join(srv, NULL);
        assert(ts.last_ack_unbind_generation == 7);
        ts_teardown(&ts);
    }
    printf("  ok test_unbind_is_valid_for_atomic_binding\n");
}

static void test_configured_backend_import_failure_is_reported(void) {
    struct test_state ts;
    ts_init(&ts);
    pthread_t srv = spawn_server(&ts, handler_import_failure);

    waywallen_display_t*     d          = make_client(&ts);
    const waywallen_vk_ctx_t invalid_vk = { 0 };
    assert(waywallen_display_bind_vulkan(d, &invalid_vk) == WAYWALLEN_OK);
    assert(begin_test_display(d, ts.sock_path, 1920, 1080) == WAYWALLEN_OK);
    assert(drive_handshake(d, 2000) == WAYWALLEN_OK);
    assert(dispatch_next_event(d) == WAYWALLEN_OK);
    assert(ts.on_binding_ready_count == 0);
    assert(waywallen_display_stream_state(d) == WAYWALLEN_STREAM_INACTIVE);
    while (waywallen_display_wants_writable(d)) {
        assert(waywallen_display_handle_writable(d) == WAYWALLEN_OK);
    }

    pthread_join(srv, NULL);
    assert(ts.last_import_failure_generation == 7);
    assert(ts.last_import_failure_kind == WAYWALLEN_BUFFER_IMPORT_FAILURE_KIND_UNSUPPORTED);
    waywallen_display_close(d);
    waywallen_display_free(d);
    ts_teardown(&ts);
    printf("  ok test_configured_backend_import_failure_is_reported\n");
}

static void test_outbox_prioritizes_lifecycle_and_replaces_state(void) {
    struct test_state ts;
    ts_init(&ts);
    pthread_t srv = spawn_server(&ts, handler_outbox_semantics);

    waywallen_display_t* d = make_client(&ts);
    assert(begin_test_display(d, ts.sock_path, 640, 480) == WAYWALLEN_OK);
    assert(drive_handshake(d, 2000) == WAYWALLEN_OK);

    int send_buffer = 1024;
    assert(setsockopt(waywallen_display_get_fd(d),
                      SOL_SOCKET,
                      SO_SNDBUF,
                      &send_buffer,
                      sizeof(send_buffer)) == 0);

    uint32_t axes_sent = 0;
    while (axes_sent < 5000 && ! waywallen_display_wants_writable(d)) {
        assert(
            waywallen_display_send_pointer_axis(
                d, 10.0f, 20.0f, 0.0f, 1.0f, WAYWALLEN_POINTER_AXIS_SOURCE_WHEEL, axes_sent, 0) ==
            WAYWALLEN_OK);
        axes_sent++;
    }
    assert(waywallen_display_wants_writable(d));
    for (uint32_t i = 0; i < 500; ++i) {
        assert(
            waywallen_display_send_pointer_axis(
                d, 10.0f, 20.0f, 0.0f, 1.0f, WAYWALLEN_POINTER_AXIS_SOURCE_WHEEL, axes_sent, 0) ==
            WAYWALLEN_OK);
        axes_sent++;
    }

    for (uint32_t i = 0; i < 100; ++i) {
        assert(waywallen_display_send_pointer_motion(d, (float)i, 20.0f, i, 0) == WAYWALLEN_OK);
        const waywallen_display_metrics_t metrics = { .width       = 640 + i,
                                                      .height      = 480,
                                                      .refresh_mhz = 60000 };
        assert(waywallen_display_set_metrics(d, &metrics) == WAYWALLEN_OK);
        uint32_t flags = i % 2 ? WAYWALLEN_WIN_HAS_ACTIVE : 0;
        assert(waywallen_display_set_window_state(d, flags) == WAYWALLEN_OK);
    }
    assert(waywallen_display_frame_release_armed(d, 7, 42) == WAYWALLEN_OK);

    while (waywallen_display_wants_writable(d)) {
        struct pollfd pfd = { .fd = waywallen_display_get_fd(d), .events = POLLOUT, .revents = 0 };
        assert(poll(&pfd, 1, 2000) == 1);
        assert(waywallen_display_handle_writable(d) == WAYWALLEN_OK);
    }

    waywallen_display_close(d);
    waywallen_display_free(d);
    pthread_join(srv, NULL);
    assert(ts.outbox_saw_critical == 1);
    assert(ts.outbox_axis_count == axes_sent);
    assert(ts.outbox_requests_before_critical < ts.outbox_axis_count);
    assert(ts.outbox_motion_count == 1);
    assert(ts.outbox_last_motion_x == 99.0f);
    assert(ts.outbox_metrics_count == 1);
    assert(ts.outbox_last_metrics_width == 739);
    assert(ts.outbox_window_count == 1);
    assert(ts.outbox_last_window_flags == WAYWALLEN_WIN_HAS_ACTIVE);
    ts_teardown(&ts);
    printf("  ok test_outbox_prioritizes_lifecycle_and_replaces_state\n");
}

static void test_buffer_generation_restarts_on_new_connection(void) {
    struct test_state ts;
    ts_init(&ts);
    pthread_t srv = spawn_server(&ts, handler_unbind_pool_one);

    waywallen_display_t* d  = make_client(&ts);
    int                  rc = begin_test_display(d, ts.sock_path, 1920, 1080);
    assert(rc == WAYWALLEN_OK);
    assert(drive_handshake(d, 2000) == WAYWALLEN_OK);
    assert(dispatch_next_event(d) == WAYWALLEN_OK);
    assert(dispatch_next_event(d) == WAYWALLEN_OK);
    waywallen_display_close(d);
    pthread_join(srv, NULL);
    ts_teardown(&ts);

    ts_init(&ts);
    srv = spawn_server(&ts, handler_bind_generation_one);
    rc  = begin_test_display(d, ts.sock_path, 1920, 1080);
    assert(rc == WAYWALLEN_OK);
    assert(drive_handshake(d, 2000) == WAYWALLEN_OK);
    assert(dispatch_next_event(d) == WAYWALLEN_OK);
    assert(ts.last_binding_buffer_generation == 1);
    assert(ts.on_disconnected_count == 0);

    waywallen_display_close(d);
    waywallen_display_free(d);
    pthread_join(srv, NULL);
    ts_teardown(&ts);
    printf("  ok test_buffer_generation_restarts_on_new_connection\n");
}

static void test_frame_release_armed_round_trip(void) {
    struct test_state ts;
    ts_init(&ts);
    pthread_t srv = spawn_server(&ts, handler_frame_release_armed);

    waywallen_display_t* d  = make_client(&ts);
    int                  rc = begin_test_display(d, ts.sock_path, 1920, 1080);
    assert(rc == WAYWALLEN_OK);
    assert(drive_handshake(d, 2000) == WAYWALLEN_OK);
    assert(waywallen_display_frame_release_armed(d, 17, 42) == WAYWALLEN_OK);
    while (waywallen_display_wants_writable(d)) {
        assert(waywallen_display_handle_writable(d) == WAYWALLEN_OK);
    }

    pthread_join(srv, NULL);
    assert(ts.last_release_armed_buffer_generation == 17);
    assert(ts.last_release_armed_seq == 42);
    waywallen_display_close(d);
    waywallen_display_free(d);
    ts_teardown(&ts);
    printf("  ok test_frame_release_armed_round_trip\n");
}

/* ------------------------------------------------------------------ */
/*  Driver                                                             */
/* ------------------------------------------------------------------ */

int main(void) {
    test_connect_to_nonexistent_socket();
    test_legacy_blocking_connect();
    test_begin_connect_immediate();
    test_full_handshake_via_async_api();
    test_presentation_snapshot_updates_and_disconnect_reset();
    test_cross_generation_state_is_protocol_error();
    test_invalid_presentation_radius_is_protocol_error();
    test_unknown_pause_effect_kind_is_protocol_error();
    test_consumer_caps_signals_linear_only_when_no_backend();
    test_partial_welcome();
    test_server_closes_during_welcome_wait();
    test_server_sends_error_event();
    test_atomic_binding_payload();
    test_invalid_stale_release_disconnects();
    test_composition_update_targets_current_binding();
    test_non_monotonic_buffer_generation_is_protocol_error();
    test_mismatched_unbind_generation_is_protocol_error();
    test_set_composition_while_idle_is_protocol_error();
    test_unbind_is_valid_for_atomic_binding();
    test_configured_backend_import_failure_is_reported();
    test_outbox_prioritizes_lifecycle_and_replaces_state();
    test_buffer_generation_restarts_on_new_connection();
    test_frame_release_armed_round_trip();
    printf("test_display: OK\n");
    return 0;
}
