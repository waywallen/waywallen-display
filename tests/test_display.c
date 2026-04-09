/*
 * End-to-end smoke test for `libwaywallen_display` at the handshake
 * layer: run a tiny mock server in a background pthread that binds a
 * tempfile UDS, waits for the library to connect, and walks the
 * `hello` → `welcome` → `register_display` → `display_accepted`
 * exchange using the same generated `ww_proto` encoders the Rust
 * daemon would use. Verify that `waywallen_display_connect` returns
 * success on the client side and that no callbacks fire that
 * shouldn't.
 *
 * Deeper (bind_buffers/frame_ready) coverage arrives in a later phase
 * once producer-side sync_fd export lands and a real renderer can
 * provide the fds. For now the handshake is the single most
 * important state-machine path to validate independently of the
 * daemon.
 */

#define _POSIX_C_SOURCE 200809L

#include "waywallen_display.h"

#include "codec.h"
#include "ww_proto.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

struct test_state {
    char sock_path[128];
    int listen_fd;

    /* Signals set by callbacks — main thread reads after connect(). */
    int on_disconnected_count;
    int on_textures_ready_count;
    int on_config_count;
    int on_frame_ready_count;
};

/* ------------------------------------------------------------------ */
/*  Callbacks                                                          */
/* ------------------------------------------------------------------ */

static void cb_textures_ready(void *ud, const waywallen_textures_t *t) {
    (void)t;
    struct test_state *ts = (struct test_state *)ud;
    ts->on_textures_ready_count++;
}
static void cb_textures_releasing(void *ud, const waywallen_textures_t *t) {
    (void)ud; (void)t;
}
static void cb_config(void *ud, const waywallen_config_t *c) {
    (void)c;
    struct test_state *ts = (struct test_state *)ud;
    ts->on_config_count++;
}
static void cb_frame_ready(void *ud, const waywallen_frame_t *f) {
    (void)f;
    struct test_state *ts = (struct test_state *)ud;
    ts->on_frame_ready_count++;
}
static void cb_disconnected(void *ud, int code, const char *msg) {
    (void)code; (void)msg;
    struct test_state *ts = (struct test_state *)ud;
    ts->on_disconnected_count++;
}

/* ------------------------------------------------------------------ */
/*  Mock server — single-shot handshake responder                      */
/* ------------------------------------------------------------------ */

static int server_run_once(int client_fd) {
    static uint8_t body_buf[WW_CODEC_MAX_BODY_BYTES];
    uint16_t op;
    size_t body_len;
    int fds[4];
    size_t n_fds;

    /* 1. Expect HELLO. */
    int rc = ww_codec_recv_request(client_fd, &op, body_buf,
                                   WW_CODEC_MAX_BODY_BYTES, &body_len,
                                   fds, 4, &n_fds);
    if (rc != 0) {
        fprintf(stderr, "[server] recv hello failed: %d\n", rc);
        return -1;
    }
    if (op != WW_REQ_HELLO) {
        fprintf(stderr, "[server] expected HELLO, got op=%u\n", op);
        return -1;
    }
    ww_req_hello_t hello;
    if (ww_req_hello_decode(body_buf, body_len, &hello) != WW_OK) {
        fprintf(stderr, "[server] decode hello failed\n");
        return -1;
    }
    if (strcmp(hello.protocol, WW_PROTOCOL_NAME) != 0) {
        fprintf(stderr, "[server] bad protocol string: %s\n", hello.protocol);
        ww_req_hello_free(&hello);
        return -1;
    }
    ww_req_hello_free(&hello);

    /* 2. Send WELCOME. */
    ww_evt_welcome_t welcome;
    memset(&welcome, 0, sizeof(welcome));
    welcome.server_version = (char *)"mock-server/0.1";
    char *features_data[1];
    features_data[0] = (char *)"explicit_sync_fd";
    welcome.features.count = 1;
    welcome.features.data = features_data;

    ww_buf_t out;
    ww_buf_init(&out);
    if (ww_evt_welcome_encode(&welcome, &out) != WW_OK) {
        fprintf(stderr, "[server] encode welcome failed\n");
        ww_buf_free(&out);
        return -1;
    }
    rc = ww_codec_send_event(client_fd, WW_EVT_WELCOME,
                             out.data, out.len, NULL, 0);
    ww_buf_free(&out);
    if (rc != 0) {
        fprintf(stderr, "[server] send welcome failed: %d\n", rc);
        return -1;
    }

    /* 3. Expect REGISTER_DISPLAY. */
    rc = ww_codec_recv_request(client_fd, &op, body_buf,
                               WW_CODEC_MAX_BODY_BYTES, &body_len,
                               fds, 4, &n_fds);
    if (rc != 0 || op != WW_REQ_REGISTER_DISPLAY) {
        fprintf(stderr, "[server] expected REGISTER_DISPLAY, got op=%u rc=%d\n",
                op, rc);
        return -1;
    }
    ww_req_register_display_t reg;
    if (ww_req_register_display_decode(body_buf, body_len, &reg) != WW_OK) {
        fprintf(stderr, "[server] decode register_display failed\n");
        return -1;
    }
    if (reg.width != 1920 || reg.height != 1080) {
        fprintf(stderr, "[server] unexpected dims: %ux%u\n",
                reg.width, reg.height);
        ww_req_register_display_free(&reg);
        return -1;
    }
    ww_req_register_display_free(&reg);

    /* 4. Send DISPLAY_ACCEPTED. */
    ww_evt_display_accepted_t accepted;
    accepted.display_id = 42;
    ww_buf_init(&out);
    if (ww_evt_display_accepted_encode(&accepted, &out) != WW_OK) {
        fprintf(stderr, "[server] encode display_accepted failed\n");
        ww_buf_free(&out);
        return -1;
    }
    rc = ww_codec_send_event(client_fd, WW_EVT_DISPLAY_ACCEPTED,
                             out.data, out.len, NULL, 0);
    ww_buf_free(&out);
    if (rc != 0) {
        fprintf(stderr, "[server] send display_accepted failed: %d\n", rc);
        return -1;
    }

    /* 5. Wait for the client's BYE (from disconnect()). */
    rc = ww_codec_recv_request(client_fd, &op, body_buf,
                               WW_CODEC_MAX_BODY_BYTES, &body_len,
                               fds, 4, &n_fds);
    if (rc == 0 && op == WW_REQ_BYE) {
        return 0;
    }
    /* If the client just closed without sending bye, that's also OK. */
    if (rc == -ECONNRESET) return 0;
    fprintf(stderr, "[server] bye wait: rc=%d op=%u\n", rc, op);
    return 0;
}

static void *server_thread_fn(void *arg) {
    struct test_state *ts = (struct test_state *)arg;

    struct sockaddr_un peer;
    socklen_t peer_len = sizeof(peer);
    int client_fd = accept(ts->listen_fd,
                           (struct sockaddr *)&peer, &peer_len);
    if (client_fd < 0) {
        fprintf(stderr, "[server] accept: %s\n", strerror(errno));
        return NULL;
    }
    (void)server_run_once(client_fd);
    close(client_fd);
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Test driver                                                        */
/* ------------------------------------------------------------------ */

int main(void) {
    struct test_state ts;
    memset(&ts, 0, sizeof(ts));
    snprintf(ts.sock_path, sizeof(ts.sock_path),
             "/tmp/waywallen-test-display-%d.sock", (int)getpid());
    unlink(ts.sock_path);

    ts.listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    assert(ts.listen_fd >= 0);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    size_t pl = strlen(ts.sock_path);
    assert(pl < sizeof(addr.sun_path));
    memcpy(addr.sun_path, ts.sock_path, pl);
    assert(bind(ts.listen_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    assert(listen(ts.listen_fd, 1) == 0);

    pthread_t srv;
    assert(pthread_create(&srv, NULL, server_thread_fn, &ts) == 0);

    waywallen_display_callbacks_t cb = {
        .on_textures_ready = cb_textures_ready,
        .on_textures_releasing = cb_textures_releasing,
        .on_config = cb_config,
        .on_frame_ready = cb_frame_ready,
        .on_disconnected = cb_disconnected,
        .user_data = &ts,
    };
    waywallen_display_t *d = waywallen_display_new(&cb);
    assert(d != NULL);

    int rc = waywallen_display_connect(d, ts.sock_path, "test-display",
                                       1920, 1080, 60000);
    assert(rc == WAYWALLEN_OK);
    printf("  ok handshake (hello/welcome/register/accepted)\n");

    /* No dispatch events were queued by the mock server after the
     * handshake, so none of the runtime callbacks should fire. */
    assert(ts.on_textures_ready_count == 0);
    assert(ts.on_config_count == 0);
    assert(ts.on_frame_ready_count == 0);
    assert(ts.on_disconnected_count == 0);

    /* An orderly disconnect sends `bye` and closes the socket. The
     * server's recv loop observes this and exits cleanly. */
    waywallen_display_disconnect(d);
    waywallen_display_destroy(d);

    pthread_join(srv, NULL);
    close(ts.listen_fd);
    unlink(ts.sock_path);

    printf("test_display: OK\n");
    return 0;
}
