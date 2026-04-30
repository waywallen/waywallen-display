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
 *   2. test_legacy_blocking_connect          — old `waywallen_display_connect` still works (poll wrapper)
 *   3. test_begin_connect_immediate          — begin_connect transitions to HELLO_PENDING on sync accept
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
    int listen_fd;

    int on_disconnected_count;
    int on_textures_ready_count;
    int on_config_count;
    int on_frame_ready_count;

    int  last_disconnect_code;
    char last_disconnect_msg[256];

    /* Populated by handler_full_handshake_capture_caps after decoding
     * the client's consumer_caps request. */
    int      saw_consumer_caps;
    uint32_t consumer_caps_mem_hints;
    uint32_t consumer_caps_sync_caps;
    uint32_t consumer_caps_color_caps;
};

static void cb_textures_ready(void *ud, const waywallen_textures_t *t) {
    (void)t;
    ((struct test_state *)ud)->on_textures_ready_count++;
}
static void cb_textures_releasing(void *ud, const waywallen_textures_t *t) {
    (void)ud; (void)t;
}
static void cb_config(void *ud, const waywallen_config_t *c) {
    (void)c;
    ((struct test_state *)ud)->on_config_count++;
}
static void cb_frame_ready(void *ud, const waywallen_frame_t *f) {
    (void)f;
    ((struct test_state *)ud)->on_frame_ready_count++;
}
static void cb_disconnected(void *ud, int code, const char *msg) {
    struct test_state *ts = (struct test_state *)ud;
    ts->on_disconnected_count++;
    ts->last_disconnect_code = code;
    if (msg) {
        snprintf(ts->last_disconnect_msg, sizeof(ts->last_disconnect_msg),
                 "%s", msg);
    } else {
        ts->last_disconnect_msg[0] = '\0';
    }
}

static const waywallen_display_callbacks_t kCallbacks = {
    .on_textures_ready = cb_textures_ready,
    .on_textures_releasing = cb_textures_releasing,
    .on_config = cb_config,
    .on_frame_ready = cb_frame_ready,
    .on_disconnected = cb_disconnected,
    .user_data = NULL,
};

static void ts_init(struct test_state *ts) {
    memset(ts, 0, sizeof(*ts));
    snprintf(ts->sock_path, sizeof(ts->sock_path),
             "/tmp/waywallen-test-display-%d-%p.sock",
             (int)getpid(), (void *)ts);
    unlink(ts->sock_path);

    ts->listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    assert(ts->listen_fd >= 0);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    size_t pl = strlen(ts->sock_path);
    assert(pl < sizeof(addr.sun_path));
    memcpy(addr.sun_path, ts->sock_path, pl);
    assert(bind(ts->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    assert(listen(ts->listen_fd, 1) == 0);
}

static void ts_teardown(struct test_state *ts) {
    if (ts->listen_fd >= 0) {
        close(ts->listen_fd);
        ts->listen_fd = -1;
    }
    unlink(ts->sock_path);
}

typedef int (*server_handler_t)(int client_fd, struct test_state *ts);

struct server_thread_arg {
    struct test_state *ts;
    server_handler_t handler;
};

static void *server_thread_fn(void *arg) {
    struct server_thread_arg *sta = (struct server_thread_arg *)arg;
    struct sockaddr_un peer;
    socklen_t peer_len = sizeof(peer);
    int client_fd = accept(sta->ts->listen_fd,
                           (struct sockaddr *)&peer, &peer_len);
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

static pthread_t spawn_server(struct test_state *ts, server_handler_t handler) {
    struct server_thread_arg *sta =
        (struct server_thread_arg *)calloc(1, sizeof(*sta));
    sta->ts = ts;
    sta->handler = handler;
    pthread_t tid;
    assert(pthread_create(&tid, NULL, server_thread_fn, sta) == 0);
    return tid;
}

static waywallen_display_t *make_client(struct test_state *ts) {
    waywallen_display_callbacks_t cb = kCallbacks;
    cb.user_data = ts;
    waywallen_display_t *d = waywallen_display_new(&cb);
    assert(d != NULL);
    return d;
}

/* Drive begin_connect → advance_handshake to completion via a private
 * poll loop. Returns WAYWALLEN_OK on DONE, or the error code from the
 * state machine. Times out at `timeout_ms`. */
static int drive_handshake(waywallen_display_t *d, int timeout_ms) {
    int fd = waywallen_display_get_fd(d);
    for (;;) {
        int rc = waywallen_display_advance_handshake(d);
        if (rc == WAYWALLEN_HS_DONE) return WAYWALLEN_OK;
        if (rc < 0) return rc;
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.revents = 0;
        if (rc == WAYWALLEN_HS_NEED_READ)       pfd.events = POLLIN;
        else if (rc == WAYWALLEN_HS_NEED_WRITE) pfd.events = POLLOUT;
        else                                    pfd.events = POLLIN | POLLOUT;
        int n = poll(&pfd, 1, timeout_ms);
        if (n == 0) return -ETIMEDOUT;
        if (n < 0) {
            if (errno == EINTR) continue;
            return -errno;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Mock server handlers                                               */
/* ------------------------------------------------------------------ */

/* Full happy-path handshake responder; mirrors what the daemon does. */
static int handler_full_handshake(int client_fd, struct test_state *ts) {
    (void)ts;
    static uint8_t body_buf[WW_CODEC_MAX_BODY_BYTES];
    uint16_t op;
    size_t body_len;
    int fds[4];
    size_t n_fds;

    int rc = ww_codec_recv_request(client_fd, &op, body_buf,
                                   WW_CODEC_MAX_BODY_BYTES, &body_len,
                                   fds, 4, &n_fds);
    if (rc != 0 || op != WW_REQ_HELLO) return -1;
    ww_req_hello_t hello;
    if (ww_req_hello_decode(body_buf, body_len, &hello) != WW_OK) return -1;
    ww_req_hello_free(&hello);

    /* Send WELCOME. */
    ww_evt_welcome_t welcome;
    memset(&welcome, 0, sizeof(welcome));
    welcome.server_version = (char *)"mock-server/0.1";
    char *features_data[1] = { (char *)"explicit_sync_fd" };
    welcome.features.count = 1;
    welcome.features.data = features_data;
    ww_buf_t out;
    ww_buf_init(&out);
    if (ww_evt_welcome_encode(&welcome, &out) != WW_OK) {
        ww_buf_free(&out);
        return -1;
    }
    rc = ww_codec_send_event(client_fd, WW_EVT_WELCOME,
                             out.data, out.len, NULL, 0);
    ww_buf_free(&out);
    if (rc != 0) return -1;

    /* Expect REGISTER_DISPLAY. */
    rc = ww_codec_recv_request(client_fd, &op, body_buf,
                               WW_CODEC_MAX_BODY_BYTES, &body_len,
                               fds, 4, &n_fds);
    if (rc != 0 || op != WW_REQ_REGISTER_DISPLAY) return -1;
    ww_req_register_display_t reg;
    if (ww_req_register_display_decode(body_buf, body_len, &reg) != WW_OK) return -1;
    ww_req_register_display_free(&reg);

    /* Send DISPLAY_ACCEPTED. */
    ww_evt_display_accepted_t accepted = { .display_id = 42 };
    ww_buf_init(&out);
    if (ww_evt_display_accepted_encode(&accepted, &out) != WW_OK) {
        ww_buf_free(&out);
        return -1;
    }
    rc = ww_codec_send_event(client_fd, WW_EVT_DISPLAY_ACCEPTED,
                             out.data, out.len, NULL, 0);
    ww_buf_free(&out);
    if (rc != 0) return -1;

    /* Drain the BYE or peer close. */
    rc = ww_codec_recv_request(client_fd, &op, body_buf,
                               WW_CODEC_MAX_BODY_BYTES, &body_len,
                               fds, 4, &n_fds);
    (void)rc;
    return 0;
}

/* Like handler_full_handshake, but also reads + decodes the
 * consumer_caps request the client emits after display_accepted, and
 * records mem_hints / sync_caps / color_caps onto the test_state for
 * the main thread to assert. */
static int handler_full_handshake_capture_caps(int client_fd,
                                               struct test_state *ts) {
    static uint8_t body_buf[WW_CODEC_MAX_BODY_BYTES];
    uint16_t op;
    size_t body_len;
    int fds[4];
    size_t n_fds;

    /* HELLO -> WELCOME */
    int rc = ww_codec_recv_request(client_fd, &op, body_buf,
                                   WW_CODEC_MAX_BODY_BYTES, &body_len,
                                   fds, 4, &n_fds);
    if (rc != 0 || op != WW_REQ_HELLO) return -1;
    ww_req_hello_t hello;
    if (ww_req_hello_decode(body_buf, body_len, &hello) != WW_OK) return -1;
    ww_req_hello_free(&hello);

    ww_evt_welcome_t welcome;
    memset(&welcome, 0, sizeof(welcome));
    welcome.server_version = (char *)"mock-server/0.1";
    char *features_data[1] = { (char *)"explicit_sync_fd" };
    welcome.features.count = 1;
    welcome.features.data = features_data;
    ww_buf_t out;
    ww_buf_init(&out);
    if (ww_evt_welcome_encode(&welcome, &out) != WW_OK) {
        ww_buf_free(&out);
        return -1;
    }
    rc = ww_codec_send_event(client_fd, WW_EVT_WELCOME,
                             out.data, out.len, NULL, 0);
    ww_buf_free(&out);
    if (rc != 0) return -1;

    /* REGISTER_DISPLAY -> DISPLAY_ACCEPTED */
    rc = ww_codec_recv_request(client_fd, &op, body_buf,
                               WW_CODEC_MAX_BODY_BYTES, &body_len,
                               fds, 4, &n_fds);
    if (rc != 0 || op != WW_REQ_REGISTER_DISPLAY) return -1;
    ww_req_register_display_t reg;
    if (ww_req_register_display_decode(body_buf, body_len, &reg) != WW_OK) return -1;
    ww_req_register_display_free(&reg);

    ww_evt_display_accepted_t accepted = { .display_id = 99 };
    ww_buf_init(&out);
    if (ww_evt_display_accepted_encode(&accepted, &out) != WW_OK) {
        ww_buf_free(&out);
        return -1;
    }
    rc = ww_codec_send_event(client_fd, WW_EVT_DISPLAY_ACCEPTED,
                             out.data, out.len, NULL, 0);
    ww_buf_free(&out);
    if (rc != 0) return -1;

    /* CONSUMER_CAPS -- the next request the client sends after
     * display_accepted. Capture mem_hints / sync_caps / color_caps. */
    rc = ww_codec_recv_request(client_fd, &op, body_buf,
                               WW_CODEC_MAX_BODY_BYTES, &body_len,
                               fds, 4, &n_fds);
    if (rc != 0 || op != WW_REQ_CONSUMER_CAPS) return -1;
    ww_req_consumer_caps_t caps;
    if (ww_req_consumer_caps_decode(body_buf, body_len, &caps) != WW_OK) {
        return -1;
    }
    ts->consumer_caps_mem_hints = caps.mem_hints;
    ts->consumer_caps_sync_caps = caps.sync_caps;
    ts->consumer_caps_color_caps = caps.color_caps;
    ts->saw_consumer_caps = 1;
    ww_req_consumer_caps_free(&caps);
    return 0;
}

/* Recv hello then send welcome in two writes (header bytes 0..1, then
 * 2..3+body) with a small pause. Forces the client's recv state machine
 * to handle a partial header. */
static int handler_partial_welcome(int client_fd, struct test_state *ts) {
    (void)ts;
    static uint8_t body_buf[WW_CODEC_MAX_BODY_BYTES];
    uint16_t op;
    size_t body_len;
    int fds[4];
    size_t n_fds;

    int rc = ww_codec_recv_request(client_fd, &op, body_buf,
                                   WW_CODEC_MAX_BODY_BYTES, &body_len,
                                   fds, 4, &n_fds);
    if (rc != 0 || op != WW_REQ_HELLO) return -1;

    /* Encode welcome body. */
    ww_evt_welcome_t welcome;
    memset(&welcome, 0, sizeof(welcome));
    welcome.server_version = (char *)"mock-server/0.1";
    char *features_data[1] = { (char *)"explicit_sync_fd" };
    welcome.features.count = 1;
    welcome.features.data = features_data;
    ww_buf_t body;
    ww_buf_init(&body);
    if (ww_evt_welcome_encode(&welcome, &body) != WW_OK) {
        ww_buf_free(&body);
        return -1;
    }

    /* Manual framing — split header across two writes with a sleep
     * between them so the kernel surfaces an EAGAIN to the client's
     * non-blocking recv between bytes 2 and 3. */
    size_t total = 4 + body.len;
    uint8_t hdr[4];
    hdr[0] = (uint8_t)(WW_EVT_WELCOME & 0xff);
    hdr[1] = (uint8_t)((WW_EVT_WELCOME >> 8) & 0xff);
    hdr[2] = (uint8_t)(total & 0xff);
    hdr[3] = (uint8_t)((total >> 8) & 0xff);

    ssize_t w = write(client_fd, hdr, 2);
    if (w != 2) { ww_buf_free(&body); return -1; }
    sleep_ms(20);
    w = write(client_fd, hdr + 2, 2);
    if (w != 2) { ww_buf_free(&body); return -1; }
    sleep_ms(20);
    if (body.len > 0) {
        w = write(client_fd, body.data, body.len);
        if ((size_t)w != body.len) { ww_buf_free(&body); return -1; }
    }
    ww_buf_free(&body);

    /* Continue handshake normally. */
    rc = ww_codec_recv_request(client_fd, &op, body_buf,
                               WW_CODEC_MAX_BODY_BYTES, &body_len,
                               fds, 4, &n_fds);
    if (rc != 0 || op != WW_REQ_REGISTER_DISPLAY) return -1;
    ww_evt_display_accepted_t accepted = { .display_id = 7 };
    ww_buf_t out;
    ww_buf_init(&out);
    if (ww_evt_display_accepted_encode(&accepted, &out) != WW_OK) {
        ww_buf_free(&out);
        return -1;
    }
    rc = ww_codec_send_event(client_fd, WW_EVT_DISPLAY_ACCEPTED,
                             out.data, out.len, NULL, 0);
    ww_buf_free(&out);
    return rc;
}

/* Recv hello then close immediately. The client should observe ECONNRESET
 * during the welcome wait and surface on_disconnected. */
static int handler_close_after_hello(int client_fd, struct test_state *ts) {
    (void)ts;
    static uint8_t body_buf[WW_CODEC_MAX_BODY_BYTES];
    uint16_t op;
    size_t body_len;
    int fds[4];
    size_t n_fds;
    int rc = ww_codec_recv_request(client_fd, &op, body_buf,
                                   WW_CODEC_MAX_BODY_BYTES, &body_len,
                                   fds, 4, &n_fds);
    (void)rc;
    return 0;  /* return -> close(client_fd) in the thread shim */
}

/* Recv hello then send WW_EVT_ERROR (XML op=7) instead of welcome. */
static int handler_send_error_after_hello(int client_fd, struct test_state *ts) {
    (void)ts;
    static uint8_t body_buf[WW_CODEC_MAX_BODY_BYTES];
    uint16_t op;
    size_t body_len;
    int fds[4];
    size_t n_fds;
    int rc = ww_codec_recv_request(client_fd, &op, body_buf,
                                   WW_CODEC_MAX_BODY_BYTES, &body_len,
                                   fds, 4, &n_fds);
    if (rc != 0 || op != WW_REQ_HELLO) return -1;

    ww_evt_error_t er;
    er.code = 42;
    er.message = (char *)"nope";
    ww_buf_t out;
    ww_buf_init(&out);
    if (ww_evt_error_encode(&er, &out) != WW_OK) {
        ww_buf_free(&out);
        return -1;
    }
    rc = ww_codec_send_event(client_fd, WW_EVT_ERROR,
                             out.data, out.len, NULL, 0);
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
    snprintf(ts.sock_path, sizeof(ts.sock_path),
             "/tmp/waywallen-nonexistent-%d.sock", (int)getpid());
    unlink(ts.sock_path);

    waywallen_display_t *d = make_client(&ts);
    int rc = waywallen_display_begin_connect(d, ts.sock_path,
                                             "test-display",
                                             1920, 1080, 60000);
    assert(rc != WAYWALLEN_OK);
    waywallen_display_destroy(d);
    printf("  ok test_connect_to_nonexistent_socket (rc=%d)\n", rc);
}

static void test_legacy_blocking_connect(void) {
    struct test_state ts;
    ts_init(&ts);
    pthread_t srv = spawn_server(&ts, handler_full_handshake);

    waywallen_display_t *d = make_client(&ts);
    int rc = waywallen_display_connect(d, ts.sock_path, "test-display",
                                       1920, 1080, 60000);
    assert(rc == WAYWALLEN_OK);
    assert(ts.on_disconnected_count == 0);
    waywallen_display_disconnect(d);
    waywallen_display_destroy(d);

    pthread_join(srv, NULL);
    ts_teardown(&ts);
    printf("  ok test_legacy_blocking_connect\n");
}

static void test_begin_connect_immediate(void) {
    struct test_state ts;
    ts_init(&ts);
    pthread_t srv = spawn_server(&ts, handler_full_handshake);

    waywallen_display_t *d = make_client(&ts);
    int rc = waywallen_display_begin_connect(d, ts.sock_path, "test-display",
                                             640, 480, 60000);
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

    waywallen_display_disconnect(d);
    waywallen_display_destroy(d);
    pthread_join(srv, NULL);
    ts_teardown(&ts);
    printf("  ok test_begin_connect_immediate\n");
}

static void test_full_handshake_via_async_api(void) {
    struct test_state ts;
    ts_init(&ts);
    pthread_t srv = spawn_server(&ts, handler_full_handshake);

    waywallen_display_t *d = make_client(&ts);
    int rc = waywallen_display_begin_connect(d, ts.sock_path, "test-display",
                                             1920, 1080, 60000);
    assert(rc == WAYWALLEN_OK);
    rc = drive_handshake(d, 2000);
    assert(rc == WAYWALLEN_OK);
    assert(waywallen_display_handshake_state(d) == WAYWALLEN_HS_READY);
    assert(waywallen_display_conn_state(d) == WAYWALLEN_CONN_CONNECTED);
    assert(ts.on_disconnected_count == 0);

    waywallen_display_disconnect(d);
    waywallen_display_destroy(d);
    pthread_join(srv, NULL);
    ts_teardown(&ts);
    printf("  ok test_full_handshake_via_async_api\n");
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

    waywallen_display_t *d = make_client(&ts);
    int rc = waywallen_display_begin_connect(d, ts.sock_path, "test-display",
                                             1920, 1080, 60000);
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

    assert((ts.consumer_caps_mem_hints & WW_MEM_HINT_HOST_VISIBLE) != 0
           && "expected HOST_VISIBLE in mem_hints");
    assert((ts.consumer_caps_mem_hints & WW_MEM_HINT_LINEAR_ONLY) != 0
           && "expected LINEAR_ONLY in mem_hints (no backend → fallback)");
    assert((ts.consumer_caps_mem_hints & WW_MEM_HINT_DEVICE_LOCAL) == 0
           && "DEVICE_LOCAL must not be advertised without Vulkan probe");
    assert((ts.consumer_caps_sync_caps
            & (WW_SYNC_SYNCOBJ_BINARY | WW_SYNC_SYNCOBJ_TIMELINE))
           == (WW_SYNC_SYNCOBJ_BINARY | WW_SYNC_SYNCOBJ_TIMELINE)
           && "sync_caps must advertise BINARY+TIMELINE");

    waywallen_display_disconnect(d);
    waywallen_display_destroy(d);
    ts_teardown(&ts);
    printf("  ok test_consumer_caps_signals_linear_only_when_no_backend "
           "(mem_hints=0x%x sync_caps=0x%x)\n",
           ts.consumer_caps_mem_hints, ts.consumer_caps_sync_caps);
}

static void test_partial_welcome(void) {
    struct test_state ts;
    ts_init(&ts);
    pthread_t srv = spawn_server(&ts, handler_partial_welcome);

    waywallen_display_t *d = make_client(&ts);
    int rc = waywallen_display_begin_connect(d, ts.sock_path, "test-display",
                                             1920, 1080, 60000);
    assert(rc == WAYWALLEN_OK);
    rc = drive_handshake(d, 2000);
    assert(rc == WAYWALLEN_OK);
    assert(ts.on_disconnected_count == 0);

    waywallen_display_disconnect(d);
    waywallen_display_destroy(d);
    pthread_join(srv, NULL);
    ts_teardown(&ts);
    printf("  ok test_partial_welcome\n");
}

static void test_server_closes_during_welcome_wait(void) {
    struct test_state ts;
    ts_init(&ts);
    pthread_t srv = spawn_server(&ts, handler_close_after_hello);

    waywallen_display_t *d = make_client(&ts);
    int rc = waywallen_display_begin_connect(d, ts.sock_path, "test-display",
                                             1920, 1080, 60000);
    assert(rc == WAYWALLEN_OK);
    rc = drive_handshake(d, 1000);
    assert(rc == WAYWALLEN_ERR_NOTCONN);
    assert(ts.on_disconnected_count == 1);

    waywallen_display_destroy(d);
    pthread_join(srv, NULL);
    ts_teardown(&ts);
    printf("  ok test_server_closes_during_welcome_wait\n");
}

static void test_server_sends_error_event(void) {
    struct test_state ts;
    ts_init(&ts);
    pthread_t srv = spawn_server(&ts, handler_send_error_after_hello);

    waywallen_display_t *d = make_client(&ts);
    int rc = waywallen_display_begin_connect(d, ts.sock_path, "test-display",
                                             1920, 1080, 60000);
    assert(rc == WAYWALLEN_OK);
    rc = drive_handshake(d, 1000);
    assert(rc == WAYWALLEN_ERR_PROTO);
    assert(ts.on_disconnected_count == 1);
    assert(strcmp(ts.last_disconnect_msg, "nope") == 0);

    waywallen_display_destroy(d);
    pthread_join(srv, NULL);
    ts_teardown(&ts);
    printf("  ok test_server_sends_error_event\n");
}

/* ------------------------------------------------------------------ */
/*  Driver                                                             */
/* ------------------------------------------------------------------ */

int main(void) {
    test_connect_to_nonexistent_socket();
    test_legacy_blocking_connect();
    test_begin_connect_immediate();
    test_full_handshake_via_async_api();
    test_consumer_caps_signals_linear_only_when_no_backend();
    test_partial_welcome();
    test_server_closes_during_welcome_wait();
    test_server_sends_error_event();
    printf("test_display: OK\n");
    return 0;
}
