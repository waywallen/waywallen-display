/*
 * libwaywallen_display — lifecycle + state machine.
 *
 * Drives the wire handshake (hello / welcome / register_display /
 * display_accepted), the post-handshake dispatch loop (bind_buffers,
 * composition config, frame_ready, unbind), and the
 * close → drain → free shutdown sequence. EGL and Vulkan backend
 * bindings (when compiled in) handle DMA-BUF import; surfaced to
 * the host as an atomic binding with the matching handle arrays.
 * All protocol / IO errors latch into DEAD and the queued
 * `on_disconnected` fires from the next host-facing entry. No code
 * path aborts or exits except `free` on undrained pending pools.
 */

#define _POSIX_C_SOURCE 200809L

#include "waywallen_display.h"

#include "codec.h"
#include "drm_fourcc_internal.h"
#include "log_file.h"
#include "ww_proto.h"

#ifdef WW_HAVE_EGL
#    include "backend_egl.h"
#endif
#ifdef WW_HAVE_VULKAN
#    include "backend_vulkan.h"
#    include "backend_vulkan_blit.h"
#endif

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/*  Library version                                                    */
/* ------------------------------------------------------------------ */

waywallen_display_version_t waywallen_display_version(void) {
    return (waywallen_display_version_t) {
        .major = WAYWALLEN_DISPLAY_VERSION_MAJOR,
        .minor = WAYWALLEN_DISPLAY_VERSION_MINOR,
        .patch = WAYWALLEN_DISPLAY_VERSION_PATCH,
    };
}

/* ------------------------------------------------------------------ */
/*  Logging                                                            */
/* ------------------------------------------------------------------ */

static waywallen_log_callback_t s_log_cb = NULL;
static void*                    s_log_ud = NULL;

void waywallen_display_set_log_callback(waywallen_log_callback_t cb, void* user_data) {
    s_log_cb = cb;
    s_log_ud = user_data;
}

void waywallen_display_set_log_tag(const char* tag) { ww_log_file_set_tag(tag); }

__attribute__((format(printf, 2, 3), visibility("hidden"))) void ww_log(waywallen_log_level_t level,
                                                                        const char* fmt, ...) {
    char    buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ww_log_file_append_for_level(level, buf);
    if (s_log_cb) {
        s_log_cb(level, buf, s_log_ud);
    } else {
        static const char* tags[] = { "DEBUG", "INFO", "WARN", "ERROR" };
        fprintf(stderr, "waywallen_display [%s] %s\n", tags[level < 4 ? level : 3], buf);
    }
}

/* Internal connection state (maps to public waywallen_conn_state_t). */
typedef enum ww_conn_state
{
    WW_CONN_DISCONNECTED = 0,
    WW_CONN_CONNECTING,
    WW_CONN_CONNECTED,
    WW_CONN_DEAD,
} ww_conn_state_t;

typedef enum ww_stream_phase
{
    WW_STREAM_IDLE = 0,
    WW_STREAM_ACTIVE,
} ww_stream_phase_t;

typedef struct ww_bound_pool_state {
    bool                valid;
    uint64_t            generation;
    ww_stream_phase_t   phase;
    waywallen_binding_t binding;
} ww_bound_pool_state_t;

/* Internal handshake state (maps to public waywallen_handshake_state_t).
 * Numerically aligned with the public enum so the accessor is a cast. */
typedef enum ww_handshake_state
{
    WW_HS_IDLE          = 0,
    WW_HS_CONNECTING    = 1,
    WW_HS_HELLO_PENDING = 2,
    WW_HS_WELCOME_WAIT  = 3,
    WW_HS_REGISTER_PEND = 4,
    WW_HS_ACCEPTED_WAIT = 5,
    WW_HS_READY         = 6,
} ww_handshake_state_t;

/* The current wire frame uses the same partial-send buffer as the
 * handshake. Post-handshake requests remain semantic until selected. */
#define WW_OUTBOX_INITIAL      4096
#define WW_OUTBOX_MAX          65536 /* one wire frame's worth (u16 length) */
#define WW_OUTBOX_NORMAL_MAX   65536
#define WW_OUTBOX_CRITICAL_MAX 16384

typedef enum ww_outbox_class
{
    WW_OUTBOX_CRITICAL,
    WW_OUTBOX_ORDERED,
    WW_OUTBOX_REPLACE_METRICS,
    WW_OUTBOX_REPLACE_WINDOW,
    WW_OUTBOX_REPLACE_MOTION,
} ww_outbox_class_t;

typedef struct ww_outbox_node {
    uint8_t*               data;
    size_t                 len;
    ww_outbox_class_t      cls;
    struct ww_outbox_node* next;
} ww_outbox_node_t;

/* Pool teardown is split: the I/O-thread bind_buffers handler enqueues
 * the previous pool's GPU resources here, and the host's render-thread
 * call to waywallen_display_drain() actually runs
 * vkDestroyImage / glDeleteTextures / etc. Decoupling the threads is
 * what closes the cross-thread race documented on the public API. */
#ifdef WW_HAVE_VULKAN
struct ww_vk_pending_pool {
    ww_vk_imported_image_t*    images;     /* count entries */
    VkSemaphore*               semaphores; /* count entries */
    uint32_t                   count;
    struct ww_vk_pending_pool* next;
};
#endif
#ifdef WW_HAVE_EGL
struct ww_egl_pending_pool {
    void**                      images;      /* EGLImageKHR; count entries */
    uint32_t*                   gl_textures; /* GLuint; count entries (zero entries skipped) */
    uint32_t                    count;
    void*                       egl_display; /* EGLDisplay snapshot, NOT owned */
    struct ww_egl_pending_pool* next;
};
#endif

struct waywallen_display {
    waywallen_display_callbacks_t cb;

    /* Connection to the backend daemon. */
    int             fd;
    ww_conn_state_t conn;
    uint64_t        display_id;

    /* Handshake state machine. Only meaningful while conn is
     * CONNECTING; reset to IDLE on disconnect/dead. */
    ww_handshake_state_t hs_state;
    /* Handshake bytes or the post-handshake frame currently selected
     * for a partial send. */
    uint8_t*              out_buf;
    size_t                out_cap; /* allocated capacity */
    size_t                out_len; /* bytes queued; out_pos..out_len is unsent */
    size_t                out_pos; /* bytes already sent into the kernel buffer */
    ww_outbox_class_t     out_class;
    bool                  out_class_valid;
    ww_outbox_node_t*     critical_head;
    ww_outbox_node_t*     critical_tail;
    ww_outbox_node_t*     ordered_head;
    ww_outbox_node_t*     ordered_tail;
    ww_outbox_node_t*     replace_metrics;
    ww_outbox_node_t*     replace_window;
    ww_outbox_node_t*     replace_motion;
    size_t                critical_bytes;
    size_t                normal_bytes;
    ww_codec_recv_state_t hs_recv;
    /* Saved register_display params, captured in begin_connect and
     * applied when WELCOME_WAIT transitions to REGISTER_PEND. */
    char hs_display_name[256];
    /* Stable per-(DE,screen) identifier used by the daemon as the key
     * into per-display settings. Empty string means "no stable id";
     * the daemon then falls back to keying by `hs_display_name`. */
    char                        hs_instance_id[128];
    waywallen_display_metrics_t hs_metrics;
    uint32_t                    window_state_flags;
    bool                        window_state_dirty_after_register;
    /* DRM render-node id of the GPU this display will sample dmabufs on.
     * Populated by waywallen_display_bind_egl/bind_vulkan via the
     * backend's introspection helpers (`ww_egl_query_drm_render_node` /
     * `ww_vk_query_drm_render_node`). `(0, 0)` if no backend is bound or
     * the driver lacks the relevant extension; the daemon then
     * conservatively assumes a cross-GPU consumer and forces
     * HOST_VISIBLE on every connected renderer. May also be set
     * explicitly via `waywallen_display_set_drm_render_node`. */
    uint32_t hs_drm_render_major;
    uint32_t hs_drm_render_minor;
    uint32_t presentation_caps;

    /* Presentation state is connection-local and independent of the
     * buffer/layout generations below. */
    waywallen_presentation_snapshot_t presentation;
    bool                              has_presentation;

    /* Backend selection. */
    waywallen_backend_t backend;
    waywallen_egl_ctx_t egl;
    waywallen_vk_ctx_t  vk;

#ifdef WW_HAVE_EGL
    /* Populated on `bind_egl` if libEGL is loadable. */
    ww_egl_backend_t egl_backend;
    uint32_t         egl_import_count;
    void**           egl_images;
    uint32_t*        egl_gl_textures;
    /* Released-but-not-yet-destroyed pools, drained by the host's
     * render thread via waywallen_display_drain. */
    struct ww_egl_pending_pool* egl_pending;
#endif
#ifdef WW_HAVE_VULKAN
    ww_vk_backend_t         vk_backend;
    VkQueue                 vk_host_queue;
    uint32_t                vk_import_count;
    ww_vk_imported_image_t* vk_images;     /* length = vk_import_count */
    VkSemaphore*            vk_semaphores; /* one per buffer slot */
    /* Released-but-not-yet-destroyed pools (see above). */
    struct ww_vk_pending_pool* vk_pending;

    /* DMABUF_RELAY only: lib-owned VkInstance/Device/Queue + a blitter
     * that imports producer DMA-BUFs into sampled VkImages and blits
     * them into a LINEAR-tiled shadow whose DMA-BUF is re-exported. */
    ww_vk_owned_t   vk_owned;
    ww_vk_blitter_t vk_blitter;
#endif

    /* Guards `vk_pending` / `egl_pending` against concurrent push from
     * the I/O thread (bind_buffers handler) and drain from the host's
     * render thread. */
    pthread_mutex_t pending_mutex;

    /* Current pool plus its protocol phase. Kept intact after a fatal
     * disconnect until the render-thread cleanup path releases it. */
    ww_bound_pool_state_t bound;
    bool                  has_last_buffer_generation;
    uint64_t              last_buffer_generation;
    bool                  has_last_config_generation;
    uint64_t              last_config_generation;
    bool                  has_failed_buffer_generation;
    uint64_t              failed_buffer_generation;

    /* Last categorised disconnect reason + a fixed-buffer copy of the
     * accompanying message. Updated by fire_disconnected_r at the
     * moment of latching, so hosts can read them synchronously from
     * inside the on_disconnected callback (which the lib fires later
     * from flush_dead_event in a host-facing entry). Reset to NONE
     * when a subsequent connect reaches CONNECTED. */
    waywallen_disconnect_reason_t last_reason;
    char                          last_message[256];
    /* Latched disconnect notification: fire_disconnected_r sets state
     * to DEAD and queues this flag; the host-facing entry that drove
     * the disconnect (dispatch / advance_handshake / handle_writable
     * / send_pointer_* etc.) calls flush_dead_event as its very last
     * action. That way `on_disconnected` always fires from a frame
     * the host can safely free `d` from — no more "callback may free
     * me, lib must not touch d after" UAF foot-gun. */
    bool dead_event_pending;
    bool presentation_reset_pending;
    int  dead_err;
};

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

static waywallen_disconnect_reason_t map_daemon_error_code(uint32_t code) {
    switch (code) {
    case WAYWALLEN_DISPLAY_ERROR_CODE_PROTOCOL_VIOLATION:
        return WAYWALLEN_DISCONNECT_PROTOCOL_ERROR;
    case WAYWALLEN_DISPLAY_ERROR_CODE_VERSION_UNSUPPORTED:
        return WAYWALLEN_DISCONNECT_VERSION_UNSUPPORTED;
    default: return WAYWALLEN_DISCONNECT_DAEMON_ERROR;
    }
}

enum
{
    WW_PRESENTATION_BLUR_RADIUS_MIN     = 1,
    WW_PRESENTATION_BLUR_RADIUS_MAX     = 64,
    WW_PRESENTATION_BLUR_RADIUS_DEFAULT = 30,
};

static waywallen_presentation_snapshot_t presentation_reset_snapshot(void) {
    waywallen_presentation_snapshot_t presentation = { 0 };
    presentation.config.pause_effect.kind          = WAYWALLEN_PAUSE_EFFECT_KIND_NONE;
    presentation.config.pause_effect.blur.radius   = WW_PRESENTATION_BLUR_RADIUS_DEFAULT;
    return presentation;
}

static void outbox_reset_queue(waywallen_display_t* d);
static int  acknowledge_frame_release(waywallen_display_t* d, uint64_t buffer_generation,
                                      uint64_t seq);
static int resolve_frame_without_gpu(waywallen_display_t* d, int release_syncobj_fd,
                                     uint64_t buffer_generation, uint64_t seq, const char* context);

static bool presentation_snapshot_valid(const waywallen_presentation_snapshot_t* presentation) {
    if (! presentation) return false;
    if (presentation->config.generation == 0 || presentation->state.generation == 0) {
        return false;
    }
    if (presentation->state.config_generation != presentation->config.generation) {
        return false;
    }
    const waywallen_pause_effect_config_t* effect = &presentation->config.pause_effect;
    if (effect->kind != WAYWALLEN_PAUSE_EFFECT_KIND_NONE &&
        effect->kind != WAYWALLEN_PAUSE_EFFECT_KIND_BLUR) {
        return false;
    }
    if (effect->blur.radius < WW_PRESENTATION_BLUR_RADIUS_MIN ||
        effect->blur.radius > WW_PRESENTATION_BLUR_RADIUS_MAX) {
        return false;
    }
    return effect->kind == WAYWALLEN_PAUSE_EFFECT_KIND_BLUR ||
           ! presentation->state.pause_effect.active;
}

/* Latch the display into DEAD state and queue an `on_disconnected`
 * notification. Does NOT call the callback — that happens via
 * flush_dead_event from a host-facing entry. Idempotent on already-
 * dead displays; the first reason wins. */
static void fire_disconnected_r(waywallen_display_t* d, waywallen_disconnect_reason_t reason,
                                int err, const char* msg) {
    if (d->conn == WW_CONN_DEAD) return;
    d->conn                       = WW_CONN_DEAD;
    d->bound.phase                = WW_STREAM_IDLE;
    d->hs_state                   = WW_HS_IDLE;
    d->display_id                 = 0;
    d->presentation_reset_pending = d->has_presentation;
    d->presentation               = presentation_reset_snapshot();
    d->has_presentation           = false;
    d->last_reason                = reason;
    if (msg) {
        size_t n = sizeof(d->last_message) - 1;
        strncpy(d->last_message, msg, n);
        d->last_message[n] = '\0';
    } else {
        d->last_message[0] = '\0';
    }
    ww_codec_recv_state_reset(&d->hs_recv);
    d->out_len = 0;
    d->out_pos = 0;
    outbox_reset_queue(d);
    if (d->fd >= 0) {
        close(d->fd);
        d->fd = -1;
    }
    d->dead_err           = err;
    d->dead_event_pending = true;
}

/* Fire the queued on_disconnected callback if any. MUST be called as
 * the very last statement of the host-facing function that drove the
 * transition: the host is allowed to `waywallen_display_free(d)`
 * from inside the callback, so any access to `d` after the callback
 * returns is a use-after-free. The caller's compiled return path
 * must not touch `d` afterwards. */
static void flush_dead_event(waywallen_display_t* d) {
    if (! d || ! d->dead_event_pending) return;
    d->dead_event_pending                          = false;
    bool presentation_reset_pending                = d->presentation_reset_pending;
    d->presentation_reset_pending                  = false;
    waywallen_display_callbacks_t     cb           = d->cb;
    int                               err          = d->dead_err;
    waywallen_presentation_snapshot_t presentation = d->presentation;
    /* Snapshot the message pointer into d's own buffer; the lib's API
     * doc says it stays valid for d's lifetime. The host MAY free d
     * from inside on_disconnected — after that, the buffer is gone. */
    const char* msg = d->last_message;
    if (presentation_reset_pending && cb.on_presentation_snapshot) {
        cb.on_presentation_snapshot(cb.user_data, &presentation);
    }
    if (cb.on_disconnected) {
        cb.on_disconnected(cb.user_data, err, msg);
    }
    /* d may have been freed by the callback. DO NOT TOUCH IT. */
}

/* Default-categorised wrapper for IO/syscall paths. Specific callsites
 * (handshake, decode, daemon error event) call fire_disconnected_r
 * directly with a more precise reason. */
static void fire_disconnected(waywallen_display_t* d, int err, const char* msg) {
    waywallen_disconnect_reason_t r;
    if (err == WAYWALLEN_ERR_NOTCONN) {
        r = WAYWALLEN_DISCONNECT_DAEMON_GONE;
    } else if (err == WAYWALLEN_ERR_IO) {
        r = WAYWALLEN_DISCONNECT_SOCKET_IO;
    } else if (err == WAYWALLEN_ERR_PROTO) {
        r = WAYWALLEN_DISCONNECT_PROTOCOL_ERROR;
    } else {
        r = WAYWALLEN_DISCONNECT_SOCKET_IO;
    }
    fire_disconnected_r(d, r, err, msg);
}

static void close_all_fds(int* fds, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (fds[i] >= 0) close(fds[i]);
    }
}

static int default_socket_path(char* out, size_t cap) {
    const char* runtime = getenv("XDG_RUNTIME_DIR");
    const char* base    = (runtime && runtime[0]) ? runtime : "/tmp";
    int         n       = snprintf(out, cap, "%s/waywallen/display.sock", base);
    if (n < 0 || (size_t)n >= cap) return WAYWALLEN_ERR_INVAL;
    return WAYWALLEN_OK;
}

/* Grow the outbox to hold at least `needed` bytes. Caps at
 * WW_OUTBOX_MAX (one wire frame's worth). Returns 0 on success or
 * -ENOMEM if we hit the cap or realloc fails. */
static int outbox_reserve(waywallen_display_t* d, size_t needed) {
    if (needed <= d->out_cap) return 0;
    if (needed > WW_OUTBOX_MAX) return -ENOMEM;
    size_t cap = d->out_cap ? d->out_cap : WW_OUTBOX_INITIAL;
    while (cap < needed) {
        if (cap >= WW_OUTBOX_MAX / 2) {
            cap = WW_OUTBOX_MAX;
            break;
        }
        cap *= 2;
    }
    uint8_t* nb = (uint8_t*)realloc(d->out_buf, cap);
    if (! nb) return -ENOMEM;
    d->out_buf = nb;
    d->out_cap = cap;
    return 0;
}

static void outbox_node_free(ww_outbox_node_t* node) {
    if (! node) return;
    free(node->data);
    free(node);
}

static void outbox_reset_queue(waywallen_display_t* d) {
    ww_outbox_node_t* lists[] = { d->critical_head, d->ordered_head };
    for (size_t i = 0; i < sizeof(lists) / sizeof(lists[0]); ++i) {
        ww_outbox_node_t* node = lists[i];
        while (node) {
            ww_outbox_node_t* next = node->next;
            outbox_node_free(node);
            node = next;
        }
    }
    outbox_node_free(d->replace_metrics);
    outbox_node_free(d->replace_window);
    outbox_node_free(d->replace_motion);
    d->critical_head = d->critical_tail = NULL;
    d->ordered_head = d->ordered_tail = NULL;
    d->replace_metrics = d->replace_window = d->replace_motion = NULL;
    d->critical_bytes = d->normal_bytes = 0;
    d->out_class_valid                  = false;
}

static bool outbox_has_queued(const waywallen_display_t* d) {
    return d->critical_head || d->ordered_head || d->replace_metrics || d->replace_window ||
           d->replace_motion;
}

static ww_outbox_node_t* outbox_pop_next(waywallen_display_t* d) {
    ww_outbox_node_t* node = d->critical_head;
    if (node) {
        d->critical_head = node->next;
        if (! d->critical_head) d->critical_tail = NULL;
        d->critical_bytes -= node->len;
    } else if ((node = d->ordered_head)) {
        d->ordered_head = node->next;
        if (! d->ordered_head) d->ordered_tail = NULL;
        d->normal_bytes -= node->len;
    } else if ((node = d->replace_metrics)) {
        d->replace_metrics = NULL;
        d->normal_bytes -= node->len;
    } else if ((node = d->replace_window)) {
        d->replace_window = NULL;
        d->normal_bytes -= node->len;
    } else if ((node = d->replace_motion)) {
        d->replace_motion = NULL;
        d->normal_bytes -= node->len;
    }
    if (node) node->next = NULL;
    return node;
}

static int outbox_load_next(waywallen_display_t* d) {
    if (d->out_pos < d->out_len) return 0;
    d->out_pos = d->out_len = 0;
    d->out_class_valid      = false;
    ww_outbox_node_t* node  = outbox_pop_next(d);
    if (! node) return 0;
    if (outbox_reserve(d, node->len) != 0) {
        outbox_node_free(node);
        return -ENOMEM;
    }
    memcpy(d->out_buf, node->data, node->len);
    d->out_len         = node->len;
    d->out_class       = node->cls;
    d->out_class_valid = true;
    outbox_node_free(node);
    return 0;
}

/* Try one non-blocking send of the highest-priority semantic request. */
static int outbox_flush_one(waywallen_display_t* d) {
    if (d->fd < 0) return -EBADF;
    int load_rc = outbox_load_next(d);
    if (load_rc != 0) return load_rc;
    if (d->out_pos >= d->out_len) return 0;
    ssize_t n = ww_codec_send_partial(d->fd, d->out_buf + d->out_pos, d->out_len - d->out_pos);
    if (n < 0) return (int)n;
    d->out_pos += (size_t)n;
    if (d->out_pos >= d->out_len) {
        d->out_pos = d->out_len = 0;
        d->out_class_valid      = false;
    }
    return (int)n;
}

static int outbox_enqueue_request(waywallen_display_t* d, ww_outbox_class_t cls, uint16_t opcode,
                                  int (*encode)(const void*, ww_buf_t*), const void* msg) {
    if (! d || d->fd < 0 || d->conn == WW_CONN_DEAD) {
        return WAYWALLEN_ERR_NOTCONN;
    }
    ww_buf_t body;
    ww_buf_init(&body);
    int rc = encode(msg, &body);
    if (rc != WW_OK) {
        ww_buf_free(&body);
        return WAYWALLEN_ERR_NOMEM;
    }
    size_t total = 4u + body.len;
    if (total > UINT16_MAX) {
        ww_buf_free(&body);
        return WAYWALLEN_ERR_INVAL;
    }
    ww_outbox_node_t* node = (ww_outbox_node_t*)calloc(1, sizeof(*node));
    if (! node || ! (node->data = (uint8_t*)malloc(total))) {
        outbox_node_free(node);
        ww_buf_free(&body);
        return WAYWALLEN_ERR_NOMEM;
    }
    node->len  = total;
    node->cls  = cls;
    uint8_t* p = node->data;
    p[0]       = (uint8_t)(opcode & 0xff);
    p[1]       = (uint8_t)((opcode >> 8) & 0xff);
    p[2]       = (uint8_t)(total & 0xff);
    p[3]       = (uint8_t)((total >> 8) & 0xff);
    if (body.len > 0) memcpy(p + 4, body.data, body.len);
    ww_buf_free(&body);

    bool replaceable = cls == WW_OUTBOX_REPLACE_METRICS || cls == WW_OUTBOX_REPLACE_WINDOW ||
                       cls == WW_OUTBOX_REPLACE_MOTION;
    if (replaceable && d->out_class_valid && d->out_class == cls && d->out_pos == 0) {
        if (outbox_reserve(d, total) != 0) {
            outbox_node_free(node);
            return WAYWALLEN_ERR_NOMEM;
        }
        memcpy(d->out_buf, node->data, total);
        d->out_len = total;
        outbox_node_free(node);
    } else if (cls == WW_OUTBOX_CRITICAL) {
        if (d->critical_bytes + total > WW_OUTBOX_CRITICAL_MAX) {
            outbox_node_free(node);
            return WAYWALLEN_ERR_NOMEM;
        }
        if (d->critical_tail)
            d->critical_tail->next = node;
        else
            d->critical_head = node;
        d->critical_tail = node;
        d->critical_bytes += total;
    } else if (cls == WW_OUTBOX_ORDERED) {
        if (d->normal_bytes + total > WW_OUTBOX_NORMAL_MAX) {
            outbox_node_free(node);
            return WAYWALLEN_ERR_NOMEM;
        }
        if (d->ordered_tail)
            d->ordered_tail->next = node;
        else
            d->ordered_head = node;
        d->ordered_tail = node;
        d->normal_bytes += total;
    } else {
        ww_outbox_node_t** slot     = cls == WW_OUTBOX_REPLACE_METRICS  ? &d->replace_metrics
                                      : cls == WW_OUTBOX_REPLACE_WINDOW ? &d->replace_window
                                                                        : &d->replace_motion;
        size_t             replaced = *slot ? (*slot)->len : 0;
        if (d->normal_bytes - replaced + total > WW_OUTBOX_NORMAL_MAX) {
            outbox_node_free(node);
            return WAYWALLEN_ERR_NOMEM;
        }
        outbox_node_free(*slot);
        *slot           = node;
        d->normal_bytes = d->normal_bytes - replaced + total;
    }

    int flush_rc = outbox_flush_one(d);
    if (flush_rc >= 0) return WAYWALLEN_OK;
    return flush_rc == -ENOMEM ? WAYWALLEN_ERR_NOMEM : WAYWALLEN_ERR_IO;
}

/* Encoder trampolines with void-pointer signature so the outbox/
 * handshake helpers can call them generically. */
static int enc_hello(const void* m, ww_buf_t* out) {
    return ww_req_hello_encode((const ww_req_hello_t*)m, out);
}
static int enc_register(const void* m, ww_buf_t* out) {
    return ww_req_register_display_encode((const ww_req_register_display_t*)m, out);
}
static int enc_set_metrics(const void* m, ww_buf_t* out) {
    return ww_req_set_display_metrics_encode((const ww_req_set_display_metrics_t*)m, out);
}
static int enc_buffer_import_failed(const void* m, ww_buf_t* out) {
    return ww_req_buffer_import_failed_encode((const ww_req_buffer_import_failed_t*)m, out);
}
static int enc_ack_unbind(const void* m, ww_buf_t* out) {
    return ww_req_ack_unbind_encode((const ww_req_ack_unbind_t*)m, out);
}

/* Consumer capability bits mirrored from the daemon's public v8 schema. */
#define WW_MEM_HINT_DEVICE_LOCAL (1u << 0)
#define WW_MEM_HINT_HOST_VISIBLE (1u << 1)
#define WW_SYNC_SYNCOBJ_BINARY   (1u << 1)
#define WW_SYNC_SYNCOBJ_TIMELINE (1u << 2)
#define WW_COLOR_ENC_SRGB        (1u << 0)
#define WW_COLOR_RANGE_LIMITED   (1u << 6)
#define WW_COLOR_ALPHA_PREMUL    (1u << 7)
/* DRM fourcc whitelist + WW_DRM_FORMAT_MOD_LINEAR live in
 * drm_fourcc_internal.h (included near the top of this file). */

/* Per-(fourcc, modifier) accumulator used while probing the active
 * backend. Grows by doubling. */
typedef struct ww_caps_buf {
    uint32_t* fourccs; /* one entry per modifier (flattened) */
    uint64_t* modifiers;
    uint32_t* plane_counts;
    size_t    n;
    size_t    cap;
    int       oom;
} ww_caps_buf_t;

typedef struct ww_consumer_caps_storage {
    waywallen_consumer_capabilities_t caps;
    ww_caps_buf_t                     flat;
    uint32_t*                         grouped_fourccs;
    uint32_t*                         grouped_counts;
    uint32_t                          device_uuid[4];
    uint32_t                          driver_uuid[4];
} ww_consumer_caps_storage_t;

static int ww_caps_buf_grow(ww_caps_buf_t* b) {
    size_t    n = b->cap ? b->cap * 2 : 16;
    uint32_t* f = (uint32_t*)malloc(n * sizeof(*f));
    uint64_t* m = (uint64_t*)malloc(n * sizeof(*m));
    uint32_t* p = (uint32_t*)malloc(n * sizeof(*p));
    if (! f || ! m || ! p) {
        free(f);
        free(m);
        free(p);
        b->oom = 1;
        return -ENOMEM;
    }
    if (b->n > 0) {
        memcpy(f, b->fourccs, b->n * sizeof(*f));
        memcpy(m, b->modifiers, b->n * sizeof(*m));
        memcpy(p, b->plane_counts, b->n * sizeof(*p));
    }
    free(b->fourccs);
    free(b->modifiers);
    free(b->plane_counts);
    b->fourccs      = f;
    b->modifiers    = m;
    b->plane_counts = p;
    b->cap          = n;
    return 0;
}

static void ww_caps_buf_emit(uint32_t fourcc, uint64_t modifier, uint32_t plane_count,
                             void* user_data) {
    ww_caps_buf_t* b = (ww_caps_buf_t*)user_data;
    if (b->oom) return;
    if (b->n >= b->cap && ww_caps_buf_grow(b) != 0) return;
    b->fourccs[b->n]      = fourcc;
    b->modifiers[b->n]    = modifier;
    b->plane_counts[b->n] = plane_count;
    b->n++;
}

static void ww_caps_buf_free(ww_caps_buf_t* b) {
    free(b->fourccs);
    free(b->modifiers);
    free(b->plane_counts);
    memset(b, 0, sizeof(*b));
}

/* Group the flattened (fourcc, modifier) pairs into the wire encoding
 * the protocol expects: distinct fourccs, with mod_counts[i] giving
 * the number of modifiers attached to fourccs[i]. Stable iteration
 * order: first occurrence of a fourcc wins. Returns 0 on success.
 * Allocates `*out_fourccs` / `*out_mod_counts`; caller frees. */
static int ww_caps_group_fourccs(const ww_caps_buf_t* flat, uint32_t** out_fourccs,
                                 uint32_t** out_mod_counts, size_t* out_n_fourccs) {
    uint32_t* fourccs = (uint32_t*)calloc(flat->n, sizeof(*fourccs));
    uint32_t* counts  = (uint32_t*)calloc(flat->n, sizeof(*counts));
    if (! fourccs || ! counts) {
        free(fourccs);
        free(counts);
        return -ENOMEM;
    }
    size_t k = 0;
    for (size_t i = 0; i < flat->n; ++i) {
        uint32_t f     = flat->fourccs[i];
        size_t   found = SIZE_MAX;
        for (size_t j = 0; j < k; ++j) {
            if (fourccs[j] == f) {
                found = j;
                break;
            }
        }
        if (found == SIZE_MAX) {
            fourccs[k] = f;
            counts[k]  = 1;
            k++;
        } else {
            counts[found]++;
        }
    }
    *out_fourccs    = fourccs;
    *out_mod_counts = counts;
    *out_n_fourccs  = k;
    return 0;
}

/* Build the capabilities embedded atomically in register_display.
 * Probes the active backend (EGL or Vulkan) for its real
 * (fourcc, modifier) import set + device UUID. Falls back to a
 * hardcoded ABGR/XRGB + LINEAR set with zero UUIDs only if the
 * probe fails or no backend is bound (the daemon's picker then
 * forces HOST_VISIBLE and treats this peer as cross-GPU). */
static int build_consumer_caps(waywallen_display_t* d, ww_consumer_caps_storage_t* storage) {
    memset(storage, 0, sizeof(*storage));
    ww_caps_buf_t* buf                    = &storage->flat;
    uint8_t        dev_uuid_bytes[16]     = { 0 };
    uint8_t        drv_uuid_bytes[16]     = { 0 };
    bool           advertise_device_local = false;

    int probe_rc = -ENOSYS;
    switch (d->backend) {
#ifdef WW_HAVE_EGL
    case WAYWALLEN_BACKEND_EGL:
        if (d->egl_backend.loaded) {
            probe_rc = ww_egl_query_format_caps(
                &d->egl_backend, (EGLDisplay)d->egl.egl_display, ww_caps_buf_emit, buf);
            /* No portable EGL/GBM way to ask "do you have DEVICE_LOCAL
             * memory I can import a dmabuf into?" — GBM consumers
             * effectively always end up in GTT (HOST_VISIBLE). Leave
             * advertise_device_local=false. */
        }
        break;
#endif
#ifdef WW_HAVE_VULKAN
    case WAYWALLEN_BACKEND_VULKAN:
    case WAYWALLEN_BACKEND_DMABUF_RELAY:
        if (d->vk_backend.loaded) {
            /* Both the direct-sampling and blit-out paths use this
             * modifier contract. */
            const uint32_t want_features =
                VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
            probe_rc =
                ww_vk_query_format_caps(&d->vk_backend, want_features, ww_caps_buf_emit, buf);
            if (probe_rc == 0) {
                /* Best-effort UUIDs; ignore failure → leave zeros. */
                (void)ww_vk_query_device_uuid(&d->vk_backend, dev_uuid_bytes, drv_uuid_bytes);
                /* If the device exposes any DEVICE_LOCAL memory type,
                 * advertise the bit so the daemon's same-device
                 * intersection (`pick_mem_hint`) can land on
                 * DEVICE_LOCAL when the producer also asks. The actual
                 * dmabuf import still re-validates memoryTypeBits at
                 * `vkAllocateMemory` time, so this hint only widens
                 * the daemon's choice — it can't force a bad mapping. */
                int has_dl = 0;
                (void)ww_vk_query_supports_device_local(&d->vk_backend, &has_dl);
                advertise_device_local = (has_dl != 0);
            }
        }
        break;
#endif
    default: break;
    }
    if (probe_rc != 0 || buf->n == 0 || buf->oom) {
        size_t probed_count = buf->n;
        ww_caps_buf_free(buf);
        ww_log(WAYWALLEN_LOG_INFO,
               "consumer_caps: backend probe unavailable (rc=%d, n=%zu); "
               "falling back to ABGR/XRGB + LINEAR",
               probe_rc,
               probed_count);
        ww_caps_buf_emit(WW_DRM_FORMAT_ABGR8888, WW_DRM_FORMAT_MOD_LINEAR, 1, buf);
        ww_caps_buf_emit(WW_DRM_FORMAT_XRGB8888, WW_DRM_FORMAT_MOD_LINEAR, 1, buf);
    } else {
        ww_log(WAYWALLEN_LOG_INFO,
               "consumer_caps: backend probe yielded %zu (fourcc, modifier) entries",
               buf->n);
    }
    if (buf->oom) {
        ww_caps_buf_free(buf);
        return -ENOMEM;
    }

    size_t grp_n = 0;
    int    gr =
        ww_caps_group_fourccs(buf, &storage->grouped_fourccs, &storage->grouped_counts, &grp_n);
    if (gr != 0) {
        ww_caps_buf_free(buf);
        return gr;
    }

    /* Pack the 16-byte UUIDs as 4×u32 little-endian for the wire. */
    for (int i = 0; i < 4; ++i) {
        memcpy(&storage->device_uuid[i], dev_uuid_bytes + i * 4, 4);
        memcpy(&storage->driver_uuid[i], drv_uuid_bytes + i * 4, 4);
    }

    waywallen_consumer_capabilities_t* caps = &storage->caps;
    caps->fourccs.count                     = (uint32_t)grp_n;
    caps->fourccs.data                      = storage->grouped_fourccs;
    caps->mod_counts.count                  = (uint32_t)grp_n;
    caps->mod_counts.data                   = storage->grouped_counts;
    caps->modifiers.count                   = (uint32_t)buf->n;
    caps->modifiers.data                    = buf->modifiers;
    caps->plane_counts.count                = (uint32_t)buf->n;
    caps->plane_counts.data                 = buf->plane_counts;
    caps->device_uuid.count                 = 4;
    caps->device_uuid.data                  = storage->device_uuid;
    caps->driver_uuid.count                 = 4;
    caps->driver_uuid.data                  = storage->driver_uuid;
    caps->drm_render_major                  = d->hs_drm_render_major;
    caps->drm_render_minor                  = d->hs_drm_render_minor;
    caps->mem_hints =
        WW_MEM_HINT_HOST_VISIBLE | (advertise_device_local ? WW_MEM_HINT_DEVICE_LOCAL : 0u);
    /* sync_caps: consumer-side release/wait always lands on the kernel
     * drm_syncobj ioctl path (`waywallen_display_signal_release_syncobj`
     * + `vk/egl_wait_sync_fd`); both BINARY and TIMELINE are always
     * supported regardless of which GPU API the host bound. No probe
     * needed. */
    caps->sync_caps = WW_SYNC_SYNCOBJ_TIMELINE | WW_SYNC_SYNCOBJ_BINARY;
    /* color_caps: encoding/range/alpha flags are interpretation-time
     * choices, not driver-locked capabilities — the consumer can
     * always sample any sRGB texture and apply any color transform
     * downstream. The defaults below are the most common desktop
     * compositor settings; daemon falls back to DEFAULT_COLOR per
     * axis if the producer's intersection is empty. No probe
     * needed. */
    caps->color_caps   = WW_COLOR_ENC_SRGB | WW_COLOR_RANGE_LIMITED | WW_COLOR_ALPHA_PREMUL;
    caps->extent_max_w = 16384;
    caps->extent_max_h = 16384;
    return 0;
}

static void consumer_caps_storage_free(ww_consumer_caps_storage_t* storage) {
    free(storage->grouped_fourccs);
    free(storage->grouped_counts);
    ww_caps_buf_free(&storage->flat);
    memset(storage, 0, sizeof(*storage));
}

/* ------------------------------------------------------------------ */
/*  Lifecycle                                                          */
/* ------------------------------------------------------------------ */

waywallen_display_t* waywallen_display_new(const waywallen_display_callbacks_t* cb) {
    if (! cb) return NULL;
    waywallen_display_t* d = (waywallen_display_t*)calloc(1, sizeof(*d));
    if (! d) return NULL;
    d->cb           = *cb;
    d->fd           = -1;
    d->conn         = WW_CONN_DISCONNECTED;
    d->bound.phase  = WW_STREAM_IDLE;
    d->backend      = WAYWALLEN_BACKEND_NONE;
    d->hs_state     = WW_HS_IDLE;
    d->presentation = presentation_reset_snapshot();
    /* 0 is a valid fd; shadow_dmabuf_fd "unset" sentinel must be -1. */
    d->bound.binding.textures.shadow_dmabuf_fd = -1;
    ww_codec_recv_state_init(&d->hs_recv);
    if (pthread_mutex_init(&d->pending_mutex, NULL) != 0) {
        free(d);
        return NULL;
    }
    return d;
}

void waywallen_display_free(waywallen_display_t* d) {
    if (! d) return;
    if (d->fd >= 0) {
        close(d->fd);
        d->fd = -1;
    }
    /* free() is intentionally NOT a GPU-resource cleanup path. The
     * backend handles (VkImage / VkDeviceMemory / VkSemaphore /
     * EGLImageKHR / GL textures) live on the host's render thread; we
     * cannot know which thread called free and therefore cannot
     * safely vkDeviceWaitIdle / vkDestroyImage / glDeleteTextures
     * from here. ABORT loudly if any pool is still bound or pending —
     * silently leaking GPU handles is worse than a crash, since the
     * driver's tracking will eventually wedge anyway. The host's
     * shutdown sequence (close → drain → free) is what gets us
     * here cleanly. */
    bool leak = false;
#ifdef WW_HAVE_VULKAN
    if (d->vk_pending) {
        uint32_t pool_n = 0, handle_n = 0;
        for (struct ww_vk_pending_pool* p = d->vk_pending; p; p = p->next) {
            pool_n++;
            handle_n += p->count;
        }
        ww_log(WAYWALLEN_LOG_ERROR,
               "free: %u Vulkan pending pool(s) (%u image+memory pairs, "
               "%u semaphores) not drained — host must call "
               "waywallen_display_drain on its render thread",
               pool_n,
               handle_n,
               handle_n);
        leak = true;
    }
    if (d->vk_import_count > 0 && (d->vk_images != NULL || d->vk_semaphores != NULL)) {
        ww_log(WAYWALLEN_LOG_ERROR,
               "free: %u Vulkan imports still bound — host must call "
               "waywallen_display_close before free",
               d->vk_import_count);
        leak = true;
    }
#endif
#ifdef WW_HAVE_EGL
    if (d->egl_pending) {
        uint32_t pool_n = 0, handle_n = 0;
        for (struct ww_egl_pending_pool* p = d->egl_pending; p; p = p->next) {
            pool_n++;
            handle_n += p->count;
        }
        ww_log(WAYWALLEN_LOG_ERROR,
               "free: %u EGL pending pool(s) (%u EGLImages + GL textures) "
               "not drained — host must call waywallen_display_drain on "
               "its render thread",
               pool_n,
               handle_n);
        leak = true;
    }
    if (d->egl_import_count > 0 && (d->egl_images != NULL || d->egl_gl_textures != NULL)) {
        ww_log(WAYWALLEN_LOG_ERROR,
               "free: %u EGL imports still bound — host must call "
               "waywallen_display_close before free",
               d->egl_import_count);
        leak = true;
    }
#endif
    if (leak) abort();

    if (d->bound.valid) {
        /* The void** arrays we built for the on_binding_ready
         * callback payload — not GPU handles, just heap, safe to
         * free here regardless of thread. */
        free(d->bound.binding.textures.vk_images);
        free(d->bound.binding.textures.vk_memories);
    }
    pthread_mutex_destroy(&d->pending_mutex);
    ww_codec_recv_state_reset(&d->hs_recv);
    outbox_reset_queue(d);
    free(d->out_buf);
    free(d);
}

int waywallen_display_bind_egl(waywallen_display_t* d, const waywallen_egl_ctx_t* ctx) {
    if (! d || ! ctx) return WAYWALLEN_ERR_INVAL;
    if (d->conn != WW_CONN_DISCONNECTED) return WAYWALLEN_ERR_STATE;
    d->backend = WAYWALLEN_BACKEND_EGL;
    d->egl     = *ctx;
#ifdef WW_HAVE_EGL
    /* Best-effort: if libEGL is on the system, resolve the function
     * pointer table now. Failure is non-fatal — the import path will
     * fall back to the NONE behavior (close incoming dma-buf fds
     * without creating textures). */
    int rc = ww_egl_backend_load(&d->egl_backend, ctx->get_proc_address);
    if (rc != 0) {
        ww_log(WAYWALLEN_LOG_WARN, "egl backend load failed: %d", rc);
        memset(&d->egl_backend, 0, sizeof(d->egl_backend));
    } else {
        ww_log(WAYWALLEN_LOG_INFO, "egl backend loaded");
        /* Auto-introspect the render node so we don't need an explicit
         * waywallen_display_set_drm_render_node call from the host. If
         * the driver lacks EGL_EXT_device_query we leave the slot at
         * (0,0) — daemon will then assume cross-GPU and force
         * HOST_VISIBLE on every renderer. */
        if (d->hs_drm_render_major == 0 && d->hs_drm_render_minor == 0) {
            uint32_t major = 0, minor = 0;
            int      qrc = ww_egl_query_drm_render_node(
                &d->egl_backend, (EGLDisplay)ctx->egl_display, &major, &minor);
            if (qrc == 0) {
                d->hs_drm_render_major = major;
                d->hs_drm_render_minor = minor;
                ww_log(WAYWALLEN_LOG_INFO, "egl drm render node = %u:%u", major, minor);
            } else {
                ww_log(WAYWALLEN_LOG_INFO,
                       "egl drm render node lookup failed (%d); reporting 0:0",
                       qrc);
            }
        }
    }
#endif
    return WAYWALLEN_OK;
}

int waywallen_display_bind_vulkan(waywallen_display_t* d, const waywallen_vk_ctx_t* ctx) {
    if (! d || ! ctx) return WAYWALLEN_ERR_INVAL;
    if (d->conn != WW_CONN_DISCONNECTED) return WAYWALLEN_ERR_STATE;
    /* vk_get_instance_proc_addr may be NULL: the backend will dlopen
     * libvulkan.so.1 and pull vkGetInstanceProcAddr from it directly. */
    d->backend = WAYWALLEN_BACKEND_VULKAN;
    d->vk      = *ctx;
#ifdef WW_HAVE_VULKAN
    int rc = ww_vk_backend_load(&d->vk_backend,
                                (VkInstance)ctx->instance,
                                (VkPhysicalDevice)ctx->physical_device,
                                (VkDevice)ctx->device,
                                ctx->queue_family_index,
                                (ww_vk_get_instance_proc_addr_fn)ctx->vk_get_instance_proc_addr,
                                false);
    if (rc != 0) {
        ww_log(WAYWALLEN_LOG_WARN, "vk backend load failed: %d", rc);
        memset(&d->vk_backend, 0, sizeof(d->vk_backend));
    } else {
        ww_log(WAYWALLEN_LOG_INFO,
               "vk backend loaded (instance=%p physical_device=%p device=%p qfi=%u)",
               ctx->instance,
               ctx->physical_device,
               ctx->device,
               ctx->queue_family_index);
        PFN_vkGetDeviceQueue get_device_queue =
            (PFN_vkGetDeviceQueue)d->vk_backend.vkGetDeviceProcAddr((VkDevice)ctx->device,
                                                                    "vkGetDeviceQueue");
        if (! get_device_queue) {
            ww_log(WAYWALLEN_LOG_WARN, "vkGetDeviceQueue unavailable");
            ww_vk_backend_unload(&d->vk_backend);
            memset(&d->vk_backend, 0, sizeof(d->vk_backend));
            d->backend = WAYWALLEN_BACKEND_NONE;
            memset(&d->vk, 0, sizeof(d->vk));
            return WAYWALLEN_ERR_NOT_IMPL;
        }
        get_device_queue((VkDevice)ctx->device, ctx->queue_family_index, 0, &d->vk_host_queue);
        if (d->vk_host_queue == VK_NULL_HANDLE) {
            ww_log(WAYWALLEN_LOG_WARN, "vkGetDeviceQueue returned NULL");
            ww_vk_backend_unload(&d->vk_backend);
            memset(&d->vk_backend, 0, sizeof(d->vk_backend));
            d->backend = WAYWALLEN_BACKEND_NONE;
            memset(&d->vk, 0, sizeof(d->vk));
            return WAYWALLEN_ERR_STATE;
        }
        if (d->hs_drm_render_major == 0 && d->hs_drm_render_minor == 0) {
            uint32_t major = 0, minor = 0;
            int      qrc = ww_vk_query_drm_render_node(&d->vk_backend, &major, &minor);
            if (qrc == 0) {
                d->hs_drm_render_major = major;
                d->hs_drm_render_minor = minor;
                ww_log(WAYWALLEN_LOG_INFO, "vk drm render node = %u:%u", major, minor);
            } else {
                ww_log(WAYWALLEN_LOG_INFO,
                       "vk drm render node lookup failed (%d); reporting 0:0",
                       qrc);
            }
        }
    }
#endif
    return WAYWALLEN_OK;
}

int waywallen_display_vulkan_requirements(waywallen_vk_requirements_t* out) {
    if (! out) return WAYWALLEN_ERR_INVAL;
#ifdef WW_HAVE_VULKAN
    *out = (waywallen_vk_requirements_t) {
        .api_version            = VK_API_VERSION_1_1,
        .device_extensions      = ww_vk_required_device_extensions,
        .device_extension_count = ww_vk_required_device_extension_count,
        .imported_image_usage =
            (uint32_t)(VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT),
        .imported_image_layout       = (uint32_t)VK_IMAGE_LAYOUT_GENERAL,
        .external_queue_family_index = VK_QUEUE_FAMILY_FOREIGN_EXT,
    };
    return WAYWALLEN_OK;
#else
    memset(out, 0, sizeof(*out));
    return WAYWALLEN_ERR_NOT_IMPL;
#endif
}

int waywallen_display_vulkan_direct_frame(waywallen_display_t* d, const waywallen_frame_t* frame,
                                          waywallen_vk_direct_frame_t* out) {
    if (! d || ! frame || ! out) return WAYWALLEN_ERR_INVAL;
    memset(out, 0, sizeof(*out));
#ifdef WW_HAVE_VULKAN
    if (d->backend != WAYWALLEN_BACKEND_VULKAN || ! d->vk_backend.loaded || ! d->bound.valid ||
        frame->buffer_generation != d->bound.generation ||
        frame->buffer_index >= d->vk_import_count || ! d->vk_images ||
        d->vk_images[frame->buffer_index].image == VK_NULL_HANDLE ||
        frame->vk_acquire_semaphore == NULL) {
        return WAYWALLEN_ERR_STATE;
    }
    const waywallen_textures_t* textures = &d->bound.binding.textures;
    out->image                           = (void*)d->vk_images[frame->buffer_index].image;
    out->format                          = (uint32_t)ww_fourcc_to_vk_format(textures->fourcc);
    out->width                           = textures->tex_width;
    out->height                          = textures->tex_height;
    out->layout                          = (uint32_t)VK_IMAGE_LAYOUT_GENERAL;
    out->external_queue_family_index     = VK_QUEUE_FAMILY_FOREIGN_EXT;
    if (out->format == (uint32_t)VK_FORMAT_UNDEFINED) {
        memset(out, 0, sizeof(*out));
        return WAYWALLEN_ERR_NOT_IMPL;
    }
    return WAYWALLEN_OK;
#else
    (void)d;
    (void)frame;
    return WAYWALLEN_ERR_NOT_IMPL;
#endif
}

int waywallen_display_vulkan_consume_frame(waywallen_display_t* d, const waywallen_frame_t* frame,
                                           waywallen_vk_sampled_frame_t* out) {
    if (! d || ! frame || ! out) return WAYWALLEN_ERR_INVAL;
    memset(out, 0, sizeof(*out));
#ifdef WW_HAVE_VULKAN
    if (d->backend != WAYWALLEN_BACKEND_VULKAN || ! d->vk_backend.loaded ||
        d->vk_host_queue == VK_NULL_HANDLE || ! d->bound.valid ||
        frame->buffer_generation != d->bound.generation ||
        frame->buffer_index >= d->vk_import_count || ! d->vk_images ||
        frame->vk_acquire_semaphore == NULL) {
        if (frame->release_syncobj_fd >= 0) {
            (void)resolve_frame_without_gpu(d,
                                            frame->release_syncobj_fd,
                                            frame->buffer_generation,
                                            frame->seq,
                                            "invalid Vulkan frame release failed");
        }
        return WAYWALLEN_ERR_STATE;
    }
    if (! ww_vk_blitter_initialized(&d->vk_blitter)) {
        int init_rc =
            ww_vk_blitter_init(&d->vk_blitter,
                               (VkInstance)d->vk.instance,
                               (VkPhysicalDevice)d->vk.physical_device,
                               (VkDevice)d->vk.device,
                               d->vk.queue_family_index,
                               d->vk_host_queue,
                               (ww_vk_get_instance_proc_addr_fn)d->vk.vk_get_instance_proc_addr);
        if (init_rc != 0) {
            if (frame->release_syncobj_fd >= 0) {
                (void)resolve_frame_without_gpu(d,
                                                frame->release_syncobj_fd,
                                                frame->buffer_generation,
                                                frame->seq,
                                                "Vulkan blitter init release failed");
            }
            return WAYWALLEN_ERR_NOT_IMPL;
        }
    }

    const waywallen_textures_t* textures        = &d->bound.binding.textures;
    bool                        candidate_ready = false;
    bool                        release_armed   = false;
    int                         rc = ww_vk_blitter_prepare(&d->vk_blitter,
                                                           d->vk_images[frame->buffer_index].image,
                                                           textures->tex_width,
                                                           textures->tex_height,
                                                           textures->fourcc,
                                                           false,
                                                           (VkSemaphore)frame->vk_acquire_semaphore,
                                                           frame->release_syncobj_fd,
                                                           &candidate_ready,
                                                           &release_armed);
    if (release_armed &&
        acknowledge_frame_release(d, frame->buffer_generation, frame->seq) != WAYWALLEN_OK) {
        if (candidate_ready) (void)ww_vk_blitter_discard_candidate(&d->vk_blitter);
        return WAYWALLEN_ERR_IO;
    }
    if (rc != 0) {
        if (candidate_ready) (void)ww_vk_blitter_discard_candidate(&d->vk_blitter);
        return WAYWALLEN_ERR_IO;
    }
    out->image     = (void*)(candidate_ready ? ww_vk_blitter_candidate(&d->vk_blitter)
                                             : ww_vk_blitter_shadow(&d->vk_blitter));
    out->format    = (uint32_t)ww_fourcc_to_vk_format(textures->fourcc);
    out->width     = textures->tex_width;
    out->height    = textures->tex_height;
    out->layout    = (uint32_t)ww_vk_blitter_shadow_layout(&d->vk_blitter);
    out->candidate = candidate_ready;
    return out->image != NULL ? WAYWALLEN_OK : WAYWALLEN_ERR_IO;
#else
    (void)d;
    (void)frame;
    return WAYWALLEN_ERR_NOT_IMPL;
#endif
}

int waywallen_display_vulkan_commit_sampled_frame(waywallen_display_t* d) {
#ifdef WW_HAVE_VULKAN
    if (! d || d->backend != WAYWALLEN_BACKEND_VULKAN ||
        ! ww_vk_blitter_initialized(&d->vk_blitter)) {
        return WAYWALLEN_ERR_STATE;
    }
    return ww_vk_blitter_commit_candidate(&d->vk_blitter) == VK_SUCCESS ? WAYWALLEN_OK
                                                                        : WAYWALLEN_ERR_IO;
#else
    (void)d;
    return WAYWALLEN_ERR_NOT_IMPL;
#endif
}

int waywallen_display_vulkan_discard_sampled_frame(waywallen_display_t* d) {
#ifdef WW_HAVE_VULKAN
    if (! d || d->backend != WAYWALLEN_BACKEND_VULKAN ||
        ! ww_vk_blitter_initialized(&d->vk_blitter)) {
        return WAYWALLEN_ERR_STATE;
    }
    return ww_vk_blitter_discard_candidate(&d->vk_blitter) == 0 ? WAYWALLEN_OK : WAYWALLEN_ERR_IO;
#else
    (void)d;
    return WAYWALLEN_ERR_NOT_IMPL;
#endif
}

int waywallen_display_bind_dmabuf_relay(waywallen_display_t* d) {
    if (! d) return WAYWALLEN_ERR_INVAL;
    if (d->conn != WW_CONN_DISCONNECTED) return WAYWALLEN_ERR_STATE;
#ifdef WW_HAVE_VULKAN
    int rc = ww_vk_create_owned(&d->vk_owned);
    if (rc == -ENOENT) return WAYWALLEN_ERR_NOMEM;
    if (rc == -ENOSYS) return WAYWALLEN_ERR_NOT_IMPL;
    if (rc != 0) return WAYWALLEN_ERR_IO;

    /* Wire vk_backend with the lib-owned handles so the existing
     * import + sync paths work unchanged. install_debug_utils=true
     * here — nobody else is mounted on this VkInstance. */
    rc = ww_vk_backend_load(&d->vk_backend,
                            d->vk_owned.instance,
                            d->vk_owned.physical_device,
                            d->vk_owned.device,
                            d->vk_owned.queue_family_index,
                            NULL, /* host_get_proc: use dlopen path */
                            true);
    if (rc != 0) {
        ww_log(WAYWALLEN_LOG_ERROR, "vk relay: backend load failed: %d", rc);
        ww_vk_destroy_owned(&d->vk_owned);
        return WAYWALLEN_ERR_NOT_IMPL;
    }

    rc = ww_vk_blitter_init(&d->vk_blitter,
                            d->vk_owned.instance,
                            d->vk_owned.physical_device,
                            d->vk_owned.device,
                            d->vk_owned.queue_family_index,
                            d->vk_owned.queue,
                            NULL);
    if (rc != 0) {
        ww_log(WAYWALLEN_LOG_ERROR, "vk relay: blitter init failed: %d", rc);
        ww_vk_backend_unload(&d->vk_backend);
        ww_vk_destroy_owned(&d->vk_owned);
        return WAYWALLEN_ERR_NOT_IMPL;
    }

    d->backend = WAYWALLEN_BACKEND_DMABUF_RELAY;
    if (d->hs_drm_render_major == 0 && d->hs_drm_render_minor == 0) {
        uint32_t major = 0, minor = 0;
        if (ww_vk_query_drm_render_node(&d->vk_backend, &major, &minor) == 0) {
            d->hs_drm_render_major = major;
            d->hs_drm_render_minor = minor;
            ww_log(WAYWALLEN_LOG_INFO, "vk relay drm render node = %u:%u", major, minor);
        }
    }
    return WAYWALLEN_OK;
#else
    (void)d;
    return WAYWALLEN_ERR_NOT_IMPL;
#endif
}

int waywallen_display_set_drm_render_node(waywallen_display_t* d, uint32_t major, uint32_t minor) {
    if (! d) return WAYWALLEN_ERR_INVAL;
    if (d->conn != WW_CONN_DISCONNECTED) return WAYWALLEN_ERR_STATE;
    d->hs_drm_render_major = major;
    d->hs_drm_render_minor = minor;
    return WAYWALLEN_OK;
}

int waywallen_display_set_presentation_caps(waywallen_display_t* d, uint32_t flags) {
    if (! d) return WAYWALLEN_ERR_INVAL;
    if (d->conn != WW_CONN_DISCONNECTED) return WAYWALLEN_ERR_STATE;
    if (flags & ~WAYWALLEN_PRESENTATION_CAP_PAUSE_BLUR) return WAYWALLEN_ERR_INVAL;
    d->presentation_caps = flags;
    return WAYWALLEN_OK;
}

/* ------------------------------------------------------------------ */
/*  Connect + handshake                                                */
/* ------------------------------------------------------------------ */

/* Open a UDS in non-blocking mode. *out_in_progress is true when
 * connect(2) returned EINPROGRESS (kernel will signal POLLOUT once
 * the connect completes). Returns fd on success, -errno on failure. */
static int open_uds_nonblock(const char* path, bool* out_in_progress) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return -errno;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    size_t pl       = strlen(path);
    if (pl >= sizeof(addr.sun_path)) {
        close(fd);
        return -ENAMETOOLONG;
    }
    memcpy(addr.sun_path, path, pl);
    *out_in_progress = false;
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        if (errno == EINPROGRESS) {
            *out_in_progress = true;
            return fd;
        }
        int e = errno;
        close(fd);
        return -e;
    }
    return fd;
}

/* Encode `msg` via `encode` into a temp ww_buf_t, then frame
 * (header + body) into d->out_buf for the partial sender to drain. */
static int hs_queue_request(waywallen_display_t* d, uint16_t                   opcode,
                            int (*encode)(const void*, ww_buf_t*), const void* msg) {
    ww_buf_t body;
    ww_buf_init(&body);
    int rc = encode(msg, &body);
    if (rc != WW_OK) {
        ww_buf_free(&body);
        return WAYWALLEN_ERR_NOMEM;
    }
    size_t total = 4 + body.len;
    if (total > UINT16_MAX || outbox_reserve(d, total) != 0) {
        ww_buf_free(&body);
        return WAYWALLEN_ERR_NOMEM;
    }
    d->out_buf[0] = (uint8_t)(opcode & 0xff);
    d->out_buf[1] = (uint8_t)((opcode >> 8) & 0xff);
    d->out_buf[2] = (uint8_t)(total & 0xff);
    d->out_buf[3] = (uint8_t)((total >> 8) & 0xff);
    if (body.len > 0) memcpy(d->out_buf + 4, body.data, body.len);
    d->out_len = total;
    d->out_pos = 0;
    ww_buf_free(&body);
    return WAYWALLEN_OK;
}

static int hs_queue_hello(waywallen_display_t* d) {
    ww_req_hello_t hello;
    memset(&hello, 0, sizeof(hello));
    waywallen_display_version_t v = waywallen_display_version();
    char                        version_buf[32];
    snprintf(version_buf, sizeof(version_buf), "%u.%u.%u", v.major, v.minor, v.patch);
    hello.client_name      = (char*)"libwaywallen_display";
    hello.client_version   = version_buf;
    hello.protocol_version = WAYWALLEN_DISPLAY_PROTOCOL_VERSION;
    return hs_queue_request(d, WW_REQ_HELLO, enc_hello, &hello);
}

static int hs_queue_register(waywallen_display_t* d) {
    ww_consumer_caps_storage_t caps;
    int                        caps_rc = build_consumer_caps(d, &caps);
    if (caps_rc != 0) return WAYWALLEN_ERR_NOMEM;
    ww_req_register_display_t reg;
    memset(&reg, 0, sizeof(reg));
    reg.name                    = d->hs_display_name;
    reg.instance_id             = d->hs_instance_id;
    reg.metrics                 = d->hs_metrics;
    reg.consumer_caps           = caps.caps;
    reg.presentation_caps.flags = d->presentation_caps;
    reg.window_state_flags      = d->window_state_flags;
    int rc                      = hs_queue_request(d, WW_REQ_REGISTER_DISPLAY, enc_register, &reg);
    consumer_caps_storage_free(&caps);
    if (rc == WAYWALLEN_OK) d->window_state_dirty_after_register = false;
    return rc;
}

static void fire_textures_releasing_if_any(waywallen_display_t* d);

int waywallen_display_begin_connect(waywallen_display_t* d, const char* socket_path,
                                    const char* display_name, const char* instance_id,
                                    const waywallen_display_metrics_t* metrics) {
    if (! d || ! display_name || ! metrics || metrics->width == 0 || metrics->height == 0) {
        return WAYWALLEN_ERR_INVAL;
    }
    if (d->conn != WW_CONN_DISCONNECTED && d->conn != WW_CONN_DEAD) {
        return WAYWALLEN_ERR_STATE;
    }
    fire_textures_releasing_if_any(d);

    char path_buf[256];
    if (! socket_path) {
        int rc = default_socket_path(path_buf, sizeof(path_buf));
        if (rc != WAYWALLEN_OK) return rc;
        socket_path = path_buf;
    }

    /* Save register_display params for the WELCOME -> REGISTER transition. */
    size_t name_len = strlen(display_name);
    if (name_len + 1 > sizeof(d->hs_display_name)) return WAYWALLEN_ERR_INVAL;
    memcpy(d->hs_display_name, display_name, name_len + 1);
    /* instance_id is optional; NULL maps to empty string ("daemon, key
     * settings by name"). */
    if (instance_id) {
        size_t iid_len = strlen(instance_id);
        if (iid_len + 1 > sizeof(d->hs_instance_id)) return WAYWALLEN_ERR_INVAL;
        memcpy(d->hs_instance_id, instance_id, iid_len + 1);
    } else {
        d->hs_instance_id[0] = '\0';
    }
    d->hs_metrics = *metrics;

    bool in_progress = false;
    int  fd          = open_uds_nonblock(socket_path, &in_progress);
    if (fd < 0) {
        return WAYWALLEN_ERR_IO;
    }
    d->fd                         = fd;
    d->conn                       = WW_CONN_CONNECTING;
    d->bound.phase                = WW_STREAM_IDLE;
    d->has_last_buffer_generation = false;
    d->last_buffer_generation     = 0;
    d->has_last_config_generation = false;
    d->last_config_generation     = 0;
    d->presentation               = presentation_reset_snapshot();
    d->has_presentation           = false;
    ww_codec_recv_state_reset(&d->hs_recv);
    d->out_len = 0;
    d->out_pos = 0;
    outbox_reset_queue(d);
    d->dead_event_pending = false;
    d->dead_err           = 0;

    if (in_progress) {
        d->hs_state = WW_HS_CONNECTING;
    } else {
        /* connect(2) finished synchronously — queue hello so the very
         * first advance call can flush it. */
        int rc = hs_queue_hello(d);
        if (rc != WAYWALLEN_OK) {
            close(d->fd);
            d->fd       = -1;
            d->conn     = WW_CONN_DEAD;
            d->hs_state = WW_HS_IDLE;
            return rc;
        }
        d->hs_state = WW_HS_HELLO_PENDING;
    }
    return WAYWALLEN_OK;
}

waywallen_handshake_state_t waywallen_display_handshake_state(waywallen_display_t* d) {
    if (! d) return WAYWALLEN_HS_IDLE;
    return (waywallen_handshake_state_t)d->hs_state;
}

/* Internal: advance one logical step. May return PROGRESS, in which
 * case advance_handshake() loops back into this function. */
static int hs_advance_one(waywallen_display_t* d) {
    switch (d->hs_state) {
    case WW_HS_IDLE:
    case WW_HS_READY: return WAYWALLEN_ERR_STATE;

    case WW_HS_CONNECTING: {
        int       err    = 0;
        socklen_t errlen = sizeof(err);
        if (getsockopt(d->fd, SOL_SOCKET, SO_ERROR, &err, &errlen) < 0) {
            fire_disconnected(d, WAYWALLEN_ERR_IO, "getsockopt SO_ERROR");
            return WAYWALLEN_ERR_IO;
        }
        if (err == EINPROGRESS) return WAYWALLEN_HS_NEED_WRITE;
        if (err != 0) {
            fire_disconnected(d, WAYWALLEN_ERR_IO, "connect failed");
            return WAYWALLEN_ERR_IO;
        }
        int rc = hs_queue_hello(d);
        if (rc != WAYWALLEN_OK) {
            fire_disconnected(d, rc, "queue hello");
            return rc;
        }
        d->hs_state = WW_HS_HELLO_PENDING;
        return WAYWALLEN_HS_PROGRESS;
    }

    case WW_HS_HELLO_PENDING:
    case WW_HS_REGISTER_PEND: {
        ssize_t n = ww_codec_send_partial(d->fd, d->out_buf + d->out_pos, d->out_len - d->out_pos);
        if (n < 0) {
            fire_disconnected(d, WAYWALLEN_ERR_IO, "send handshake");
            return WAYWALLEN_ERR_IO;
        }
        if (n == 0) return WAYWALLEN_HS_NEED_WRITE;
        d->out_pos += (size_t)n;
        if (d->out_pos < d->out_len) return WAYWALLEN_HS_NEED_WRITE;
        if (d->hs_state == WW_HS_HELLO_PENDING) {
            d->hs_state = WW_HS_WELCOME_WAIT;
        } else {
            d->hs_state = WW_HS_ACCEPTED_WAIT;
        }
        d->out_len = 0;
        d->out_pos = 0;
        return WAYWALLEN_HS_PROGRESS;
    }

    case WW_HS_WELCOME_WAIT: {
        int rc = ww_codec_recv_partial(d->fd, &d->hs_recv);
        if (rc == WW_CODEC_FRAME_NEED) return WAYWALLEN_HS_NEED_READ;
        if (rc < 0) {
            int werr = (rc == -ECONNRESET) ? WAYWALLEN_ERR_NOTCONN : WAYWALLEN_ERR_IO;
            fire_disconnected(d, werr, "recv welcome");
            return werr;
        }
        /* No fds expected on welcome; defensively close any. */
        close_all_fds(d->hs_recv.fds, d->hs_recv.n_fds);
        d->hs_recv.n_fds = 0;
        if (d->hs_recv.op == WW_EVT_ERROR) {
            ww_evt_error_t er;
            const char*    msg = "server error";
            if (ww_evt_error_decode(d->hs_recv.body, d->hs_recv.body_len, &er) == WW_OK) {
                if (er.message) msg = er.message;
                /* Capture code BEFORE fire_disconnected_r — the
                 * callback may free d via destroy(), but `er` is on
                 * our stack and safe to free unconditionally below. */
                uint32_t code = er.code;
                fire_disconnected_r(d, map_daemon_error_code(code), WAYWALLEN_ERR_PROTO, msg);
                ww_evt_error_free(&er);
            } else {
                fire_disconnected_r(d,
                                    WAYWALLEN_DISCONNECT_DAEMON_ERROR,
                                    WAYWALLEN_ERR_PROTO,
                                    "server error (decode failed)");
            }
            /* fire_disconnected_r already reset hs_recv internally;
             * d may have been freed by the callback so we MUST NOT
             * touch it here. */
            return WAYWALLEN_ERR_PROTO;
        }
        if (d->hs_recv.op != WW_EVT_WELCOME) {
            fire_disconnected_r(
                d, WAYWALLEN_DISCONNECT_HANDSHAKE_FAILED, WAYWALLEN_ERR_PROTO, "expected welcome");
            return WAYWALLEN_ERR_PROTO;
        }
        /* Decode purely for diagnostics — `welcome` is informational
         * in v3+. Version compatibility was already enforced by the
         * daemon before it sent welcome; reaching here means we are
         * a supported client. Features are advisory only. */
        ww_evt_welcome_t welcome;
        if (ww_evt_welcome_decode(d->hs_recv.body, d->hs_recv.body_len, &welcome) != WW_OK) {
            fire_disconnected_r(
                d, WAYWALLEN_DISCONNECT_HANDSHAKE_FAILED, WAYWALLEN_ERR_PROTO, "decode welcome");
            return WAYWALLEN_ERR_PROTO;
        }
        ww_evt_welcome_free(&welcome);
        ww_codec_recv_state_reset(&d->hs_recv);
        int rc2 = hs_queue_register(d);
        if (rc2 != WAYWALLEN_OK) {
            fire_disconnected(d, rc2, "queue register_display");
            return rc2;
        }
        d->hs_state = WW_HS_REGISTER_PEND;
        return WAYWALLEN_HS_PROGRESS;
    }

    case WW_HS_ACCEPTED_WAIT: {
        int rc = ww_codec_recv_partial(d->fd, &d->hs_recv);
        if (rc == WW_CODEC_FRAME_NEED) return WAYWALLEN_HS_NEED_READ;
        if (rc < 0) {
            int werr = (rc == -ECONNRESET) ? WAYWALLEN_ERR_NOTCONN : WAYWALLEN_ERR_IO;
            fire_disconnected(d, werr, "recv display_accepted");
            return werr;
        }
        close_all_fds(d->hs_recv.fds, d->hs_recv.n_fds);
        d->hs_recv.n_fds = 0;
        if (d->hs_recv.op == WW_EVT_ERROR) {
            ww_evt_error_t er;
            const char*    msg = "server error";
            if (ww_evt_error_decode(d->hs_recv.body, d->hs_recv.body_len, &er) == WW_OK) {
                if (er.message) msg = er.message;
                uint32_t code = er.code;
                fire_disconnected_r(d, map_daemon_error_code(code), WAYWALLEN_ERR_PROTO, msg);
                ww_evt_error_free(&er);
            } else {
                fire_disconnected_r(d,
                                    WAYWALLEN_DISCONNECT_DAEMON_ERROR,
                                    WAYWALLEN_ERR_PROTO,
                                    "server error (decode failed)");
            }
            /* d may be freed; do not touch. */
            return WAYWALLEN_ERR_PROTO;
        }
        if (d->hs_recv.op != WW_EVT_DISPLAY_ACCEPTED) {
            fire_disconnected_r(d,
                                WAYWALLEN_DISCONNECT_HANDSHAKE_FAILED,
                                WAYWALLEN_ERR_PROTO,
                                "expected display_accepted");
            return WAYWALLEN_ERR_PROTO;
        }
        ww_evt_display_accepted_t accepted;
        if (ww_evt_display_accepted_decode(d->hs_recv.body, d->hs_recv.body_len, &accepted) !=
            WW_OK) {
            fire_disconnected_r(d,
                                WAYWALLEN_DISCONNECT_HANDSHAKE_FAILED,
                                WAYWALLEN_ERR_PROTO,
                                "decode display_accepted");
            /* d may be freed; do not touch. */
            return WAYWALLEN_ERR_PROTO;
        }
        d->display_id = accepted.display_id;
        if (! presentation_snapshot_valid(&accepted.presentation)) {
            ww_evt_display_accepted_free(&accepted);
            fire_disconnected_r(d,
                                WAYWALLEN_DISCONNECT_HANDSHAKE_FAILED,
                                WAYWALLEN_ERR_PROTO,
                                "invalid display_accepted presentation snapshot");
            return WAYWALLEN_ERR_PROTO;
        }
        d->presentation     = accepted.presentation;
        d->has_presentation = true;
        ww_evt_display_accepted_free(&accepted);
        d->conn            = WW_CONN_CONNECTED;
        d->last_reason     = WAYWALLEN_DISCONNECT_NONE;
        d->last_message[0] = '\0';
        d->hs_state        = WW_HS_READY;
        ww_codec_recv_state_reset(&d->hs_recv);
        if (d->window_state_dirty_after_register) {
            d->window_state_dirty_after_register = false;
            int update_rc = waywallen_display_set_window_state(d, d->window_state_flags);
            if (update_rc != WAYWALLEN_OK) {
                fire_disconnected(d, update_rc, "queue post-register window state");
                return update_rc;
            }
        }
        if (d->cb.on_presentation_snapshot) {
            d->cb.on_presentation_snapshot(d->cb.user_data, &d->presentation);
        }
        return WAYWALLEN_HS_DONE;
    }
    }
    return WAYWALLEN_ERR_STATE;
}

int waywallen_display_advance_handshake(waywallen_display_t* d) {
    if (! d) return WAYWALLEN_ERR_INVAL;
    if (d->conn != WW_CONN_CONNECTING) return WAYWALLEN_ERR_NOTCONN;
    /* Loop while the state machine reports PROGRESS so the caller only
     * sees terminal codes (DONE / NEED_* / error). Bounded by the
     * number of state transitions (≤4). */
    int rc;
    for (;;) {
        rc = hs_advance_one(d);
        if (rc == WAYWALLEN_HS_PROGRESS) continue;
        break;
    }
    /* MUST be the last touch on d — flush_dead_event may invoke a
     * host callback that frees d. */
    flush_dead_event(d);
    return rc;
}

int waywallen_display_connect(waywallen_display_t* d, const char* socket_path,
                              const char* display_name, const char* instance_id,
                              const waywallen_display_metrics_t* metrics) {
    int rc = waywallen_display_begin_connect(d, socket_path, display_name, instance_id, metrics);
    if (rc != WAYWALLEN_OK) return rc;
    int fd = waywallen_display_get_fd(d);
    for (;;) {
        rc = waywallen_display_advance_handshake(d);
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
        int n = poll(&pfd, 1, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            return WAYWALLEN_ERR_IO;
        }
    }
}

int waywallen_display_set_metrics(waywallen_display_t*               d,
                                  const waywallen_display_metrics_t* metrics) {
    if (! d || ! metrics || metrics->width == 0 || metrics->height == 0) {
        return WAYWALLEN_ERR_INVAL;
    }
    if (d->conn != WW_CONN_CONNECTED) return WAYWALLEN_ERR_STATE;
    d->hs_metrics                        = *metrics;
    ww_req_set_display_metrics_t request = { .metrics = *metrics };
    return outbox_enqueue_request(
        d, WW_OUTBOX_REPLACE_METRICS, WW_REQ_SET_DISPLAY_METRICS, enc_set_metrics, &request);
}

static int enc_window_state(const void* m, ww_buf_t* out) {
    return ww_req_set_window_state_encode((const ww_req_set_window_state_t*)m, out);
}

static int enc_frame_release_armed(const void* m, ww_buf_t* out) {
    return ww_req_frame_release_armed_encode((const ww_req_frame_release_armed_t*)m, out);
}

int waywallen_display_frame_release_armed(waywallen_display_t* d, uint64_t buffer_generation,
                                          uint64_t seq) {
    if (! d) return WAYWALLEN_ERR_INVAL;
    if (d->conn != WW_CONN_CONNECTED) return WAYWALLEN_ERR_STATE;
    ww_req_frame_release_armed_t msg = {
        .buffer_generation = buffer_generation,
        .seq               = seq,
    };
    return outbox_enqueue_request(
        d, WW_OUTBOX_CRITICAL, WW_REQ_FRAME_RELEASE_ARMED, enc_frame_release_armed, &msg);
}

int waywallen_display_set_window_state(waywallen_display_t* d, uint32_t flags) {
    if (! d) return WAYWALLEN_ERR_INVAL;
    if (flags & ~WAYWALLEN_WIN_STATE_MASK) return WAYWALLEN_ERR_INVAL;
    d->window_state_flags = flags;
    if (d->conn == WW_CONN_DISCONNECTED || d->conn == WW_CONN_DEAD) return WAYWALLEN_OK;
    if (d->conn == WW_CONN_CONNECTING) {
        if (d->hs_state >= WW_HS_REGISTER_PEND) d->window_state_dirty_after_register = true;
        return WAYWALLEN_OK;
    }
    ww_req_set_window_state_t msg = { .flags = flags };
    return outbox_enqueue_request(
        d, WW_OUTBOX_REPLACE_WINDOW, WW_REQ_SET_WINDOW_STATE, enc_window_state, &msg);
}

static int enc_pointer_motion(const void* m, ww_buf_t* out) {
    return ww_req_pointer_motion_encode((const ww_req_pointer_motion_t*)m, out);
}
static int enc_pointer_button(const void* m, ww_buf_t* out) {
    return ww_req_pointer_button_encode((const ww_req_pointer_button_t*)m, out);
}
static int enc_pointer_axis(const void* m, ww_buf_t* out) {
    return ww_req_pointer_axis_encode((const ww_req_pointer_axis_t*)m, out);
}

int waywallen_display_send_pointer_motion(waywallen_display_t* d, float x, float y,
                                          uint64_t timestamp_us, uint32_t modifiers) {
    if (! d) return WAYWALLEN_ERR_INVAL;
    if (d->conn != WW_CONN_CONNECTED) return WAYWALLEN_ERR_STATE;
    if (! isfinite(x) || ! isfinite(y) || modifiers & ~WAYWALLEN_POINTER_MOD_MASK)
        return WAYWALLEN_ERR_INVAL;
    ww_req_pointer_motion_t msg = { x, y, timestamp_us, modifiers };
    return outbox_enqueue_request(
        d, WW_OUTBOX_REPLACE_MOTION, WW_REQ_POINTER_MOTION, enc_pointer_motion, &msg);
}

int waywallen_display_send_pointer_button(waywallen_display_t* d, float x, float y, uint32_t button,
                                          waywallen_pointer_button_state_t state,
                                          uint64_t timestamp_us, uint32_t modifiers) {
    if (! d) return WAYWALLEN_ERR_INVAL;
    if (d->conn != WW_CONN_CONNECTED) return WAYWALLEN_ERR_STATE;
    if (! isfinite(x) || ! isfinite(y) || modifiers & ~WAYWALLEN_POINTER_MOD_MASK ||
        (state != WAYWALLEN_POINTER_BUTTON_STATE_RELEASED &&
         state != WAYWALLEN_POINTER_BUTTON_STATE_PRESSED)) {
        return WAYWALLEN_ERR_INVAL;
    }
    ww_req_pointer_button_t msg = { x, y, button, state, timestamp_us, modifiers };
    return outbox_enqueue_request(
        d, WW_OUTBOX_ORDERED, WW_REQ_POINTER_BUTTON, enc_pointer_button, &msg);
}

int waywallen_display_send_pointer_axis(waywallen_display_t* d, float x, float y, float delta_x,
                                        float delta_y, waywallen_pointer_axis_source_t source,
                                        uint64_t timestamp_us, uint32_t modifiers) {
    if (! d) return WAYWALLEN_ERR_INVAL;
    if (d->conn != WW_CONN_CONNECTED) return WAYWALLEN_ERR_STATE;
    if (! isfinite(x) || ! isfinite(y) || ! isfinite(delta_x) || ! isfinite(delta_y) ||
        modifiers & ~WAYWALLEN_POINTER_MOD_MASK ||
        (source != WAYWALLEN_POINTER_AXIS_SOURCE_WHEEL &&
         source != WAYWALLEN_POINTER_AXIS_SOURCE_FINGER &&
         source != WAYWALLEN_POINTER_AXIS_SOURCE_CONTINUOUS)) {
        return WAYWALLEN_ERR_INVAL;
    }
    ww_req_pointer_axis_t msg = { x, y, delta_x, delta_y, source, timestamp_us, modifiers };
    return outbox_enqueue_request(
        d, WW_OUTBOX_ORDERED, WW_REQ_POINTER_AXIS, enc_pointer_axis, &msg);
}

int waywallen_display_get_fd(waywallen_display_t* d) {
    if (! d) return -1;
    return d->fd;
}

bool waywallen_display_wants_writable(waywallen_display_t* d) {
    if (! d || d->fd < 0 || d->conn == WW_CONN_DEAD) return false;
    /* During handshake, the state machine (driven by advance_handshake)
     * owns POLLOUT arming via its NEED_WRITE return code; report false
     * here so the host does not double-arm. Post-handshake (READY or
     * the brief CONNECTED-pre-READY window when registration may
     * still be queued) the outbox owns it. */
    if (d->hs_state != WW_HS_IDLE && d->hs_state != WW_HS_READY) {
        return false;
    }
    return d->out_pos < d->out_len || outbox_has_queued(d);
}

int waywallen_display_handle_writable(waywallen_display_t* d) {
    if (! d) return WAYWALLEN_ERR_INVAL;
    if (d->fd < 0 || d->conn == WW_CONN_DEAD) return WAYWALLEN_ERR_NOTCONN;
    int sent = outbox_flush_one(d);
    int ret  = WAYWALLEN_OK;
    if (sent < 0) {
        fire_disconnected(d, WAYWALLEN_ERR_IO, "outbox flush");
        ret = WAYWALLEN_ERR_IO;
    }
    flush_dead_event(d); /* must be last; may free d */
    return ret;
}

/* ------------------------------------------------------------------ */
/*  Dispatch                                                           */
/* ------------------------------------------------------------------ */

#ifdef WW_HAVE_EGL
/* Destroy a snapshotted pool's EGL/GL resources. Caller owns `p`'s
 * heap allocation; we free the embedded arrays here, free `p` itself
 * outside. Must be called from a thread with the host's GL context
 * current (glDeleteTextures requires it). */
static void egl_destroy_pending_pool_inplace(waywallen_display_t*        d,
                                             struct ww_egl_pending_pool* p) {
    if (! p) return;
    if (d->egl_backend.loaded) {
        if (p->gl_textures) {
            d->egl_backend.glDeleteTextures((int)p->count, p->gl_textures);
        }
        if (p->images && p->egl_display) {
            for (uint32_t i = 0; i < p->count; i++) {
                if (p->images[i]) {
                    ww_egl_destroy_image(
                        &d->egl_backend, (EGLDisplay)p->egl_display, (EGLImageKHR)p->images[i]);
                }
            }
        }
    }
    free(p->images);
    free(p->gl_textures);
}

/* Move the current EGL pool onto the pending-destroy queue. Runs on
 * the I/O thread (bind_buffers / unbind / disconnect handlers) — the
 * actual destruction is deferred to
 * `waywallen_display_drain` so it happens on the
 * thread holding the host's GL context. */
static void egl_release_current_pool(waywallen_display_t* d) {
    if (d->egl_import_count == 0 || (! d->egl_images && ! d->egl_gl_textures)) {
        free(d->egl_images);
        free(d->egl_gl_textures);
        d->egl_images       = NULL;
        d->egl_gl_textures  = NULL;
        d->egl_import_count = 0;
        return;
    }
    struct ww_egl_pending_pool* p = (struct ww_egl_pending_pool*)calloc(1, sizeof(*p));
    if (! p) {
        /* Allocation failed: leak the GL/EGL handles rather than
         * destroying them on the wrong thread (silent UB). The host's
         * waywallen_display_free leak-detect path will catch any
         * in-band leak via subsequent drains. */
        ww_log(WAYWALLEN_LOG_WARN,
               "egl_release_current_pool: pending alloc failed; "
               "leaking %u EGLImages / GL textures",
               d->egl_import_count);
        free(d->egl_images);
        free(d->egl_gl_textures);
        d->egl_images       = NULL;
        d->egl_gl_textures  = NULL;
        d->egl_import_count = 0;
        return;
    }
    p->images           = d->egl_images;
    p->gl_textures      = d->egl_gl_textures;
    p->count            = d->egl_import_count;
    p->egl_display      = d->egl.egl_display;
    d->egl_images       = NULL;
    d->egl_gl_textures  = NULL;
    d->egl_import_count = 0;

    pthread_mutex_lock(&d->pending_mutex);
    p->next        = d->egl_pending;
    d->egl_pending = p;
    unsigned len   = 0;
    for (struct ww_egl_pending_pool* cur = d->egl_pending; cur; cur = cur->next) len++;
    pthread_mutex_unlock(&d->pending_mutex);
    /* Visibility: host should drain on its render thread every frame.
     * If the list grows beyond a few entries the host either stopped
     * calling drain or its render thread is stuck. */
    if (len > 4) {
        ww_log(WAYWALLEN_LOG_WARN,
               "egl pending pool list grew to %u — host render thread "
               "is not draining",
               len);
    }
}
#endif

#ifdef WW_HAVE_VULKAN
/* Destroy a snapshotted pool's Vulkan resources. Caller is responsible
 * for guaranteeing no in-flight VkQueueSubmit references the handles
 * (host's render thread, post fence-wait) — we deliberately do NOT
 * vkDeviceWaitIdle here since that's the call that races with
 * vkQueueSubmit elsewhere. */
static void vk_destroy_pending_pool_inplace(waywallen_display_t* d, struct ww_vk_pending_pool* p) {
    if (! p) return;
    if (d->vk_backend.loaded) {
        if (p->images) {
            for (uint32_t i = 0; i < p->count; i++) {
                ww_vk_destroy_imported_image(&d->vk_backend, &p->images[i]);
            }
        }
        if (p->semaphores) {
            for (uint32_t i = 0; i < p->count; i++) {
                if (p->semaphores[i] != VK_NULL_HANDLE) {
                    d->vk_backend.vkDestroySemaphore(d->vk_backend.device, p->semaphores[i], NULL);
                }
            }
        }
    }
    free(p->images);
    free(p->semaphores);
}

/* Move the current Vulkan pool onto the pending-destroy queue. Runs
 * on the I/O thread; deferred destruction is what closes the race
 * with the host's render-thread vkQueueSubmit. */
static void vk_release_current_pool(waywallen_display_t* d) {
    if (d->vk_import_count == 0 || (! d->vk_images && ! d->vk_semaphores)) {
        free(d->vk_images);
        free(d->vk_semaphores);
        d->vk_images       = NULL;
        d->vk_semaphores   = NULL;
        d->vk_import_count = 0;
        return;
    }
    struct ww_vk_pending_pool* p = (struct ww_vk_pending_pool*)calloc(1, sizeof(*p));
    if (! p) {
        /* Same fallback rationale as the EGL path: leaking the imports
         * is preferable to destroying them on the wrong thread, where
         * vkDestroyImage may race with an in-flight vkQueueSubmit. */
        ww_log(WAYWALLEN_LOG_WARN,
               "vk_release_current_pool: pending alloc failed; "
               "leaking %u imported VkImages / semaphores",
               d->vk_import_count);
        free(d->vk_images);
        free(d->vk_semaphores);
        d->vk_images       = NULL;
        d->vk_semaphores   = NULL;
        d->vk_import_count = 0;
        return;
    }
    p->images          = d->vk_images;
    p->semaphores      = d->vk_semaphores;
    p->count           = d->vk_import_count;
    d->vk_images       = NULL;
    d->vk_semaphores   = NULL;
    d->vk_import_count = 0;

    pthread_mutex_lock(&d->pending_mutex);
    p->next       = d->vk_pending;
    d->vk_pending = p;
    unsigned len  = 0;
    for (struct ww_vk_pending_pool* cur = d->vk_pending; cur; cur = cur->next) len++;
    pthread_mutex_unlock(&d->pending_mutex);
    if (len > 4) {
        ww_log(WAYWALLEN_LOG_WARN,
               "vk pending pool list grew to %u — host render thread "
               "is not draining",
               len);
    }
}
#endif

int waywallen_display_drain(waywallen_display_t* d) {
    if (! d) return WAYWALLEN_ERR_INVAL;
    int drained = 0;

#ifdef WW_HAVE_VULKAN
    struct ww_vk_pending_pool* vk_head = NULL;
#endif
#ifdef WW_HAVE_EGL
    struct ww_egl_pending_pool* egl_head = NULL;
#endif

    /* Snapshot both lists under the lock, then destroy outside it so
     * driver calls (which can be slow) don't block the I/O thread's
     * next push. */
    pthread_mutex_lock(&d->pending_mutex);
#ifdef WW_HAVE_VULKAN
    vk_head       = d->vk_pending;
    d->vk_pending = NULL;
#endif
#ifdef WW_HAVE_EGL
    egl_head       = d->egl_pending;
    d->egl_pending = NULL;
#endif
    pthread_mutex_unlock(&d->pending_mutex);

#ifdef WW_HAVE_VULKAN
    while (vk_head) {
        struct ww_vk_pending_pool* next = vk_head->next;
        vk_destroy_pending_pool_inplace(d, vk_head);
        free(vk_head);
        vk_head = next;
        drained++;
    }
#endif
#ifdef WW_HAVE_EGL
    while (egl_head) {
        struct ww_egl_pending_pool* next = egl_head->next;
        egl_destroy_pending_pool_inplace(d, egl_head);
        free(egl_head);
        egl_head = next;
        drained++;
    }
#endif
    return drained;
}

static void fire_textures_releasing_if_any(waywallen_display_t* d) {
    if (! d->bound.valid) return;
    if (d->cb.on_textures_releasing) {
        d->cb.on_textures_releasing(d->cb.user_data, &d->bound.binding.textures);
    }
#ifdef WW_HAVE_EGL
    egl_release_current_pool(d);
#endif
#ifdef WW_HAVE_VULKAN
    vk_release_current_pool(d);
#endif
    /* Free the void** handle arrays we built for the callback payload. */
    free(d->bound.binding.textures.vk_images);
    free(d->bound.binding.textures.vk_memories);
    memset(&d->bound.binding, 0, sizeof(d->bound.binding));
    /* Re-arm the sentinel after the memset zeroes it. */
    d->bound.binding.textures.shadow_dmabuf_fd = -1;
    d->bound.valid                             = false;
    d->bound.generation                        = 0;
    d->bound.phase                             = WW_STREAM_IDLE;
}

#ifdef WW_HAVE_EGL
/*
 * Attempt to import the N dma-buf fds as N EGLImages + N GL textures
 * using the cached backend function table. Returns 0 on success (and
 * populates d->egl_images / d->egl_gl_textures / d->egl_import_count),
 * negative on failure (and leaves the display's EGL pool empty). On
 * failure the caller is responsible for closing the fds. On success
 * the fds are closed by this function before return — the EGL
 * driver has dup2'd them internally.
 */
static int try_egl_import(waywallen_display_t* d, const ww_evt_bind_buffers_t* bb, int* fd_buf,
                          size_t n_fds, void*** out_images, uint32_t** out_gl_textures) {
    if (! d->egl_backend.loaded) return -ENOSYS;
    if (bb->planes_per_buffer == 0 || bb->planes_per_buffer > WW_EGL_MAX_PLANES) {
        return -EINVAL;
    }
    if (bb->count == 0) return -EINVAL;
    if (bb->stride.count != n_fds || bb->plane_offset.count != n_fds) {
        return -EINVAL;
    }

    void**    images      = (void**)calloc(bb->count, sizeof(void*));
    uint32_t* gl_textures = (uint32_t*)calloc(bb->count, sizeof(uint32_t));
    if (! images || ! gl_textures) {
        free(images);
        free(gl_textures);
        return -ENOMEM;
    }

    for (uint32_t b = 0; b < bb->count; b++) {
        ww_egl_dmabuf_import_t im;
        memset(&im, 0, sizeof(im));
        im.egl_display = d->egl.egl_display;
        im.fourcc      = bb->fourcc;
        im.width       = bb->width;
        im.height      = bb->height;
        im.modifier    = bb->modifier;
        im.n_planes    = bb->planes_per_buffer;
        for (uint32_t p = 0; p < bb->planes_per_buffer; p++) {
            size_t idx    = (size_t)b * bb->planes_per_buffer + p;
            im.fds[p]     = fd_buf[idx];
            im.strides[p] = bb->stride.data[idx];
            im.offsets[p] = bb->plane_offset.data[idx];
        }
        EGLImageKHR img;
        int         rc = ww_egl_import_dmabuf(&d->egl_backend, &im, &img);
        if (rc != 0) {
            for (uint32_t j = 0; j < b; j++) {
                if (images[j]) {
                    ww_egl_destroy_image(
                        &d->egl_backend, d->egl.egl_display, (EGLImageKHR)images[j]);
                }
            }
            free(images);
            free(gl_textures);
            return rc;
        }
        images[b] = (void*)img;
        /* GL texture creation is deferred to the host's render thread
         * via waywallen_display_create_gl_texture(). */
    }

    close_all_fds(fd_buf, n_fds);
    *out_images      = images;
    *out_gl_textures = gl_textures;
    return 0;
}
#endif /* WW_HAVE_EGL */

#ifdef WW_HAVE_VULKAN
static int try_vk_import(waywallen_display_t* d, const ww_evt_bind_buffers_t* bb, int* fd_buf,
                         size_t n_fds, ww_vk_imported_image_t** out_images,
                         VkSemaphore** out_semaphores) {
    if (! d->vk_backend.loaded) return -ENOSYS;
    if (bb->planes_per_buffer == 0 || bb->planes_per_buffer > WW_VK_MAX_PLANES) return -EINVAL;
    if (bb->count == 0) return -EINVAL;
    if (bb->stride.count != n_fds || bb->plane_offset.count != n_fds) return -EINVAL;

    ww_vk_imported_image_t* images =
        (ww_vk_imported_image_t*)calloc(bb->count, sizeof(ww_vk_imported_image_t));
    VkSemaphore* semaphores = (VkSemaphore*)calloc(bb->count, sizeof(VkSemaphore));
    if (! images || ! semaphores) {
        free(images);
        free(semaphores);
        return -ENOMEM;
    }
    int failure = -EIO;

    /* Create one semaphore per slot for sync_fd import. */
    for (uint32_t b = 0; b < bb->count; b++) {
        VkSemaphoreCreateInfo sci = {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        };
        VkResult vr =
            d->vk_backend.vkCreateSemaphore(d->vk_backend.device, &sci, NULL, &semaphores[b]);
        if (vr != VK_SUCCESS) {
            if (vr == VK_ERROR_OUT_OF_HOST_MEMORY || vr == VK_ERROR_OUT_OF_DEVICE_MEMORY) {
                failure = -ENOMEM;
            }
            goto rollback;
        }
    }

    for (uint32_t b = 0; b < bb->count; b++) {
        ww_vk_dmabuf_import_t im;
        memset(&im, 0, sizeof(im));
        im.fourcc   = bb->fourcc;
        im.width    = bb->width;
        im.height   = bb->height;
        im.modifier = bb->modifier;
        im.n_planes = bb->planes_per_buffer;
        for (uint32_t p = 0; p < bb->planes_per_buffer; p++) {
            size_t idx    = (size_t)b * bb->planes_per_buffer + p;
            im.fds[p]     = fd_buf[idx];
            im.strides[p] = bb->stride.data[idx];
            im.offsets[p] = bb->plane_offset.data[idx];
        }
        int rc = ww_vk_import_dmabuf(&d->vk_backend, &im, &images[b]);
        if (rc != 0) {
            failure = rc;
            goto rollback;
        }

        size_t base  = (size_t)b * bb->planes_per_buffer;
        fd_buf[base] = -1;
        for (uint32_t p = 1; p < bb->planes_per_buffer; p++) {
            size_t idx = base + p;
            if (fd_buf[idx] >= 0) close(fd_buf[idx]);
            fd_buf[idx] = -1;
        }
    }

    *out_images     = images;
    *out_semaphores = semaphores;
    return 0;

rollback:
    for (uint32_t j = 0; j < bb->count; j++) {
        ww_vk_destroy_imported_image(&d->vk_backend, &images[j]);
        if (semaphores[j] != VK_NULL_HANDLE) {
            d->vk_backend.vkDestroySemaphore(d->vk_backend.device, semaphores[j], NULL);
        }
    }
    free(images);
    free(semaphores);
    return failure;
}
#endif /* WW_HAVE_VULKAN */

static bool composition_config_valid(const waywallen_composition_config_t* config,
                                     uint64_t expected_buffer_generation,
                                     uint64_t last_config_generation) {
    if (! config || config->generation == 0 || config->generation <= last_config_generation ||
        config->buffer_generation != expected_buffer_generation || config->transform > 7) {
        return false;
    }
    const ww_rect_t rects[] = { config->source_rect, config->dest_rect };
    for (size_t i = 0; i < sizeof(rects) / sizeof(rects[0]); ++i) {
        if (! isfinite(rects[i].x) || ! isfinite(rects[i].y) || ! isfinite(rects[i].w) ||
            ! isfinite(rects[i].h) || rects[i].w <= 0.0f || rects[i].h <= 0.0f) {
            return false;
        }
    }
    const waywallen_rgba_color_t* color = &config->clear_color;
    return isfinite(color->r) && isfinite(color->g) && isfinite(color->b) && isfinite(color->a);
}

static waywallen_buffer_import_failure_kind_t import_failure_kind(int rc) {
    if (rc == -ENOMEM) return WAYWALLEN_BUFFER_IMPORT_FAILURE_KIND_RESOURCE_EXHAUSTED;
    if (rc == -EINVAL || rc == -ENOSYS || rc == -ENOTSUP) {
        return WAYWALLEN_BUFFER_IMPORT_FAILURE_KIND_UNSUPPORTED;
    }
    return WAYWALLEN_BUFFER_IMPORT_FAILURE_KIND_BACKEND_FAILURE;
}

static int report_buffer_import_failure(waywallen_display_t* d, uint64_t generation, int rc,
                                        const char* backend_name) {
    char message[160];
    snprintf(message,
             sizeof(message),
             "%s import failed: %s (%d)",
             backend_name,
             strerror(rc < 0 ? -rc : rc),
             rc);
    ww_req_buffer_import_failed_t request = {
        .buffer_generation = generation,
        .kind              = import_failure_kind(rc),
        .message           = message,
    };
    int enqueue_rc = outbox_enqueue_request(
        d, WW_OUTBOX_CRITICAL, WW_REQ_BUFFER_IMPORT_FAILED, enc_buffer_import_failed, &request);
    if (enqueue_rc != WAYWALLEN_OK) {
        fire_disconnected(d, enqueue_rc, "buffer_import_failed enqueue failed");
        return enqueue_rc;
    }
    d->has_failed_buffer_generation = true;
    d->failed_buffer_generation     = generation;
    return WAYWALLEN_OK;
}

static int handle_bind_buffers(waywallen_display_t* d, const uint8_t* body, size_t body_len,
                               int* fd_buf, size_t n_fds) {
    ww_evt_bind_buffers_t bb;
    if (ww_evt_bind_buffers_decode(body, body_len, &bb) != WW_OK) {
        close_all_fds(fd_buf, n_fds);
        fire_disconnected(d, WAYWALLEN_ERR_PROTO, "decode bind_buffers");
        return WAYWALLEN_ERR_PROTO;
    }
    if (d->has_last_buffer_generation && bb.buffer_generation <= d->last_buffer_generation) {
        close_all_fds(fd_buf, n_fds);
        ww_evt_bind_buffers_free(&bb);
        fire_disconnected(d, WAYWALLEN_ERR_PROTO, "non-monotonic buffer_generation");
        return WAYWALLEN_ERR_PROTO;
    }

    uint64_t last_config_generation = d->has_last_config_generation ? d->last_config_generation : 0;
    if (! composition_config_valid(
            &bb.initial_config, bb.buffer_generation, last_config_generation)) {
        close_all_fds(fd_buf, n_fds);
        ww_evt_bind_buffers_free(&bb);
        fire_disconnected(d, WAYWALLEN_ERR_PROTO, "invalid initial composition config");
        return WAYWALLEN_ERR_PROTO;
    }
    ww_log(WAYWALLEN_LOG_INFO,
           "bind_buffers received gen=%" PRIu64 " count=%u %ux%u "
           "fourcc=0x%08x modifier=0x%" PRIx64 " planes_per_buffer=%u",
           bb.buffer_generation,
           bb.count,
           bb.width,
           bb.height,
           bb.fourcc,
           bb.modifier,
           bb.planes_per_buffer);
    uint32_t expected = bb.count * bb.planes_per_buffer;
    if ((size_t)expected != n_fds) {
        close_all_fds(fd_buf, n_fds);
        ww_evt_bind_buffers_free(&bb);
        fire_disconnected(d, WAYWALLEN_ERR_PROTO, "bind_buffers fd count mismatch");
        return WAYWALLEN_ERR_PROTO;
    }

    d->last_buffer_generation     = bb.buffer_generation;
    d->has_last_buffer_generation = true;

    waywallen_textures_t textures = { 0 };
    textures.shadow_dmabuf_fd     = -1;
    int         import_rc         = 0;
    const char* backend_name      = "headless";
#ifdef WW_HAVE_EGL
    void**    candidate_egl_images      = NULL;
    uint32_t* candidate_egl_gl_textures = NULL;
#endif
#ifdef WW_HAVE_VULKAN
    ww_vk_imported_image_t* candidate_vk_images     = NULL;
    VkSemaphore*            candidate_vk_semaphores = NULL;
    void**                  reported_vk_images      = NULL;
    void**                  reported_vk_memories    = NULL;
#endif

#ifdef WW_HAVE_EGL
    if (d->backend == WAYWALLEN_BACKEND_EGL) {
        backend_name = "EGL";
        import_rc    = try_egl_import(
            d, &bb, fd_buf, n_fds, &candidate_egl_images, &candidate_egl_gl_textures);
        if (import_rc == 0) {
            ww_log(WAYWALLEN_LOG_INFO,
                   "EGL import: %u images, %ux%u fourcc=0x%x",
                   bb.count,
                   bb.width,
                   bb.height,
                   bb.fourcc);
            textures.backend     = WAYWALLEN_BACKEND_EGL;
            textures.egl_images  = candidate_egl_images;
            textures.gl_textures = candidate_egl_gl_textures;
        } else {
            ww_log(WAYWALLEN_LOG_WARN, "EGL import failed: %d", import_rc);
        }
    }
#else
    if (d->backend == WAYWALLEN_BACKEND_EGL) {
        backend_name = "EGL";
        import_rc    = -ENOSYS;
    }
#endif
#ifdef WW_HAVE_VULKAN
    if (d->backend == WAYWALLEN_BACKEND_VULKAN) {
        backend_name = "Vulkan";
        import_rc =
            try_vk_import(d, &bb, fd_buf, n_fds, &candidate_vk_images, &candidate_vk_semaphores);
        if (import_rc == 0) {
            ww_log(WAYWALLEN_LOG_INFO,
                   "Vulkan import: %u images, %ux%u fourcc=0x%x",
                   bb.count,
                   bb.width,
                   bb.height,
                   bb.fourcc);
            reported_vk_images   = (void**)calloc(bb.count, sizeof(void*));
            reported_vk_memories = (void**)calloc(bb.count, sizeof(void*));
            if (! reported_vk_images || ! reported_vk_memories) {
                import_rc = -ENOMEM;
            } else {
                for (uint32_t i = 0; i < bb.count; i++) {
                    reported_vk_images[i]   = (void*)candidate_vk_images[i].image;
                    reported_vk_memories[i] = (void*)candidate_vk_images[i].memory;
                }
                textures.backend     = WAYWALLEN_BACKEND_VULKAN;
                textures.vk_images   = reported_vk_images;
                textures.vk_memories = reported_vk_memories;
            }
        } else {
            ww_log(WAYWALLEN_LOG_WARN, "Vulkan import failed: %d", import_rc);
        }
    }
    if (d->backend == WAYWALLEN_BACKEND_DMABUF_RELAY) {
        backend_name = "DMA-BUF relay";
        import_rc =
            try_vk_import(d, &bb, fd_buf, n_fds, &candidate_vk_images, &candidate_vk_semaphores);
        if (import_rc == 0) {
            VkFormat shadow_fmt = ww_fourcc_to_vk_format(bb.fourcc);
            import_rc           = ww_vk_blitter_ensure_shadow_exportable(
                &d->vk_blitter, bb.width, bb.height, shadow_fmt);
            if (import_rc == 0) {
                int      sfd                               = -1;
                uint32_t sn                                = 0;
                uint32_t sstr[WAYWALLEN_DMABUF_MAX_PLANES] = { 0 };
                uint64_t soff[WAYWALLEN_DMABUF_MAX_PLANES] = { 0 };
                uint64_t smod                              = 0;
                if (ww_vk_blitter_get_export(&d->vk_blitter, &sfd, &sn, sstr, soff, &smod) == 0) {
                    textures.shadow_dmabuf_fd = sfd;
                    textures.shadow_n_planes  = sn;
                    for (uint32_t i = 0; i < sn && i < WAYWALLEN_DMABUF_MAX_PLANES; i++) {
                        textures.shadow_strides[i] = sstr[i];
                        textures.shadow_offsets[i] = soff[i];
                    }
                    textures.shadow_modifier = smod;
                } else {
                    import_rc = -EIO;
                }
                ww_log(WAYWALLEN_LOG_INFO,
                       "dmabuf_relay: shadow ready %ux%u fourcc=0x%x fd=%d",
                       bb.width,
                       bb.height,
                       bb.fourcc,
                       sfd);
                if (import_rc == 0) textures.backend = WAYWALLEN_BACKEND_DMABUF_RELAY;
            } else {
                ww_log(WAYWALLEN_LOG_WARN,
                       "dmabuf_relay: ensure_shadow_exportable failed: %d",
                       import_rc);
            }
        } else {
            ww_log(WAYWALLEN_LOG_WARN, "dmabuf_relay: producer import failed: %d", import_rc);
        }
    }
#else
    if (d->backend == WAYWALLEN_BACKEND_VULKAN || d->backend == WAYWALLEN_BACKEND_DMABUF_RELAY) {
        backend_name = d->backend == WAYWALLEN_BACKEND_VULKAN ? "Vulkan" : "DMA-BUF relay";
        import_rc    = -ENOSYS;
    }
#endif

    if (d->backend == WAYWALLEN_BACKEND_NONE) {
        close_all_fds(fd_buf, n_fds);
        textures.backend = WAYWALLEN_BACKEND_NONE;
    } else if (import_rc != 0) {
        close_all_fds(fd_buf, n_fds);
#ifdef WW_HAVE_EGL
        if (candidate_egl_images) {
            for (uint32_t i = 0; i < bb.count; ++i) {
                if (candidate_egl_images[i]) {
                    ww_egl_destroy_image(
                        &d->egl_backend, d->egl.egl_display, (EGLImageKHR)candidate_egl_images[i]);
                }
            }
        }
        free(candidate_egl_images);
        free(candidate_egl_gl_textures);
#endif
#ifdef WW_HAVE_VULKAN
        if (candidate_vk_images || candidate_vk_semaphores) {
            for (uint32_t i = 0; i < bb.count; ++i) {
                if (candidate_vk_images) {
                    ww_vk_destroy_imported_image(&d->vk_backend, &candidate_vk_images[i]);
                }
                if (candidate_vk_semaphores && candidate_vk_semaphores[i] != VK_NULL_HANDLE) {
                    d->vk_backend.vkDestroySemaphore(
                        d->vk_backend.device, candidate_vk_semaphores[i], NULL);
                }
            }
        }
        free(candidate_vk_images);
        free(candidate_vk_semaphores);
        free(reported_vk_images);
        free(reported_vk_memories);
#endif
        uint64_t failed_generation = bb.buffer_generation;
        ww_evt_bind_buffers_free(&bb);
        return report_buffer_import_failure(d, failed_generation, import_rc, backend_name);
    }

    textures.count             = bb.count;
    textures.tex_width         = bb.width;
    textures.tex_height        = bb.height;
    textures.fourcc            = bb.fourcc;
    textures.modifier          = bb.modifier;
    textures.planes_per_buffer = bb.planes_per_buffer;
    textures.buffer_generation = bb.buffer_generation;

    fire_textures_releasing_if_any(d);
#ifdef WW_HAVE_EGL
    d->egl_images       = candidate_egl_images;
    d->egl_gl_textures  = candidate_egl_gl_textures;
    d->egl_import_count = d->backend == WAYWALLEN_BACKEND_EGL ? bb.count : 0;
#endif
#ifdef WW_HAVE_VULKAN
    d->vk_images     = candidate_vk_images;
    d->vk_semaphores = candidate_vk_semaphores;
    d->vk_import_count =
        (d->backend == WAYWALLEN_BACKEND_VULKAN || d->backend == WAYWALLEN_BACKEND_DMABUF_RELAY)
            ? bb.count
            : 0;
#endif
    d->bound.generation             = bb.buffer_generation;
    d->bound.binding.textures       = textures;
    d->bound.binding.config         = bb.initial_config;
    d->bound.valid                  = true;
    d->bound.phase                  = WW_STREAM_ACTIVE;
    d->last_config_generation       = bb.initial_config.generation;
    d->has_last_config_generation   = true;
    d->has_failed_buffer_generation = false;

    ww_evt_bind_buffers_free(&bb);
    if (d->cb.on_binding_ready) {
        d->cb.on_binding_ready(d->cb.user_data, &d->bound.binding);
    }
    return WAYWALLEN_OK;
}

static int handle_set_composition_config(waywallen_display_t* d, const uint8_t* body,
                                         size_t body_len) {
    ww_evt_set_composition_config_t event;
    if (ww_evt_set_composition_config_decode(body, body_len, &event) != WW_OK) {
        fire_disconnected(d, WAYWALLEN_ERR_PROTO, "decode set_composition_config");
        return WAYWALLEN_ERR_PROTO;
    }
    uint64_t last_generation = d->has_last_config_generation ? d->last_config_generation : 0;
    if (! d->bound.valid ||
        ! composition_config_valid(&event.config, d->bound.generation, last_generation)) {
        ww_evt_set_composition_config_free(&event);
        fire_disconnected(d, WAYWALLEN_ERR_PROTO, "invalid composition config");
        return WAYWALLEN_ERR_PROTO;
    }
    d->bound.binding.config       = event.config;
    d->last_config_generation     = event.config.generation;
    d->has_last_config_generation = true;
    ww_evt_set_composition_config_free(&event);

    if (d->cb.on_composition_config) {
        d->cb.on_composition_config(d->cb.user_data, &d->bound.binding.config);
    }
    return WAYWALLEN_OK;
}

static int handle_set_presentation_snapshot(waywallen_display_t* d, const uint8_t* body,
                                            size_t body_len) {
    ww_evt_set_presentation_snapshot_t event;
    if (ww_evt_set_presentation_snapshot_decode(body, body_len, &event) != WW_OK) {
        fire_disconnected(d, WAYWALLEN_ERR_PROTO, "decode set_presentation_snapshot");
        return WAYWALLEN_ERR_PROTO;
    }
    bool valid = d->has_presentation && presentation_snapshot_valid(&event.presentation) &&
                 event.presentation.config.generation > d->presentation.config.generation &&
                 event.presentation.state.generation > d->presentation.state.generation;
    if (! valid) {
        ww_evt_set_presentation_snapshot_free(&event);
        fire_disconnected(d, WAYWALLEN_ERR_PROTO, "invalid presentation snapshot");
        return WAYWALLEN_ERR_PROTO;
    }
    d->presentation = event.presentation;
    ww_evt_set_presentation_snapshot_free(&event);
    if (d->cb.on_presentation_snapshot) {
        d->cb.on_presentation_snapshot(d->cb.user_data, &d->presentation);
    }
    return WAYWALLEN_OK;
}

static int handle_set_presentation_state(waywallen_display_t* d, const uint8_t* body,
                                         size_t body_len) {
    ww_evt_set_presentation_state_t event;
    if (ww_evt_set_presentation_state_decode(body, body_len, &event) != WW_OK) {
        fire_disconnected(d, WAYWALLEN_ERR_PROTO, "decode set_presentation_state");
        return WAYWALLEN_ERR_PROTO;
    }
    const waywallen_presentation_state_t* state = &event.state;
    bool valid = d->has_presentation && state->generation > d->presentation.state.generation &&
                 state->config_generation == d->presentation.config.generation &&
                 (d->presentation.config.pause_effect.kind == WAYWALLEN_PAUSE_EFFECT_KIND_BLUR ||
                  ! state->pause_effect.active);
    if (! valid) {
        ww_evt_set_presentation_state_free(&event);
        fire_disconnected(d, WAYWALLEN_ERR_PROTO, "invalid presentation state");
        return WAYWALLEN_ERR_PROTO;
    }
    d->presentation.state = *state;
    ww_evt_set_presentation_state_free(&event);
    if (d->cb.on_presentation_state) {
        d->cb.on_presentation_state(d->cb.user_data, &d->presentation.state);
    }
    return WAYWALLEN_OK;
}

static int acknowledge_frame_release(waywallen_display_t* d, uint64_t buffer_generation,
                                     uint64_t seq) {
    int rc = waywallen_display_frame_release_armed(d, buffer_generation, seq);
    if (rc != WAYWALLEN_OK) {
        fire_disconnected(d, rc, "frame_release_armed enqueue failed");
    }
    return rc;
}

static int resolve_frame_without_gpu(waywallen_display_t* d, int release_syncobj_fd,
                                     uint64_t buffer_generation, uint64_t seq,
                                     const char* context) {
    int rc = waywallen_display_signal_release_syncobj(release_syncobj_fd);
    if (rc != WAYWALLEN_OK) {
        fire_disconnected(d, rc, context);
        return rc;
    }
    return acknowledge_frame_release(d, buffer_generation, seq);
}

static int handle_frame_ready(waywallen_display_t* d, const uint8_t* body, size_t body_len,
                              int* fd_buf, size_t n_fds) {
    ww_evt_frame_ready_t fr;
    if (ww_evt_frame_ready_decode(body, body_len, &fr) != WW_OK) {
        close_all_fds(fd_buf, n_fds);
        fire_disconnected(d, WAYWALLEN_ERR_PROTO, "decode frame_ready");
        return WAYWALLEN_ERR_PROTO;
    }
    /* v1: 2 fds — [0] acquire sync_fd, [1] release_syncobj fd. */
    if (n_fds != 2) {
        close_all_fds(fd_buf, n_fds);
        ww_evt_frame_ready_free(&fr);
        fire_disconnected(d, WAYWALLEN_ERR_PROTO, "frame_ready expected 2 fds");
        return WAYWALLEN_ERR_PROTO;
    }
    int acquire_fd         = fd_buf[0];
    int release_syncobj_fd = fd_buf[1];
    /* A frame from the retired pool can still be queued behind a newer
     * BindBuffers. It is stale regardless of the new pool's config state. */
    if (! d->bound.valid || fr.buffer_generation != d->bound.generation) {
        close(acquire_fd);
        int rc = resolve_frame_without_gpu(
            d, release_syncobj_fd, fr.buffer_generation, fr.seq, "stale frame release failed");
        if (rc != WAYWALLEN_OK) {
            ww_evt_frame_ready_free(&fr);
            return rc;
        }
        ww_evt_frame_ready_free(&fr);
        return WAYWALLEN_OK;
    }
    if (d->bound.phase != WW_STREAM_ACTIVE) {
        close(acquire_fd);
        (void)resolve_frame_without_gpu(
            d, release_syncobj_fd, fr.buffer_generation, fr.seq, "pre-config frame release failed");
        ww_evt_frame_ready_free(&fr);
        fire_disconnected(d, WAYWALLEN_ERR_PROTO, "frame_ready before binding");
        return WAYWALLEN_ERR_PROTO;
    }

    int   fd_handled        = 0;
    int   release_armed     = 0;
    void* acquire_semaphore = NULL;

#ifdef WW_HAVE_EGL
    if (! fd_handled && d->backend == WAYWALLEN_BACKEND_EGL && d->egl_backend.loaded) {
        int rc = ww_egl_wait_sync_fd(&d->egl_backend, d->egl.egl_display, acquire_fd);
        if (rc != 0) {
            close(acquire_fd);
        }
        fd_handled = 1;
    }
#endif
#ifdef WW_HAVE_VULKAN
    if (! fd_handled && d->backend == WAYWALLEN_BACKEND_VULKAN && d->vk_backend.loaded &&
        d->vk_semaphores) {
        uint32_t slot = fr.buffer_index;
        if (slot < d->vk_import_count && d->vk_semaphores[slot] != VK_NULL_HANDLE) {
            int rc = ww_vk_import_sync_fd(&d->vk_backend, d->vk_semaphores[slot], acquire_fd);
            if (rc == 0) {
                acquire_semaphore = (void*)d->vk_semaphores[slot];
            } else {
                close(acquire_fd);
            }
        } else {
            close(acquire_fd);
        }
        fd_handled = 1;
    }
    if (! fd_handled && d->backend == WAYWALLEN_BACKEND_DMABUF_RELAY && d->vk_backend.loaded &&
        d->vk_semaphores && ww_vk_blitter_initialized(&d->vk_blitter)) {
        uint32_t    slot    = fr.buffer_index;
        VkSemaphore acq_sem = VK_NULL_HANDLE;
        if (slot < d->vk_import_count && d->vk_semaphores[slot] != VK_NULL_HANDLE) {
            int rc = ww_vk_import_sync_fd(&d->vk_backend, d->vk_semaphores[slot], acquire_fd);
            if (rc == 0) {
                acq_sem = d->vk_semaphores[slot];
            } else {
                close(acquire_fd);
            }
        } else {
            close(acquire_fd);
        }
        if (slot < d->vk_import_count && d->vk_images[slot].image != VK_NULL_HANDLE) {
            /* Blit consumes release_syncobj_fd (signals + close) on its
             * own success/failure paths — pass ownership through. */
            bool armed    = false;
            int  blit_rc  = ww_vk_blitter_blit(&d->vk_blitter,
                                               d->vk_images[slot].image,
                                               d->bound.binding.textures.tex_width,
                                               d->bound.binding.textures.tex_height,
                                               acq_sem,
                                               release_syncobj_fd,
                                               &armed);
            release_armed = armed ? 1 : 0;
            if (blit_rc != 0 && ! armed) {
                ww_vk_blitter_shutdown(&d->vk_blitter);
                ww_evt_frame_ready_free(&fr);
                fire_disconnected(d, WAYWALLEN_ERR_IO, "relay frame release could not be armed");
                return WAYWALLEN_ERR_IO;
            }
        } else if (release_syncobj_fd >= 0) {
            /* No image to blit — still need to release. */
            int rc = waywallen_display_signal_release_syncobj(release_syncobj_fd);
            if (rc != WAYWALLEN_OK) {
                ww_evt_frame_ready_free(&fr);
                fire_disconnected(d, rc, "relay missing-image release failed");
                return rc;
            }
            release_armed = 1;
        }
        /* The lib already drove sync to completion; host sees no sem +
         * no release fd to deal with. */
        release_syncobj_fd = -1;
        fd_handled         = 1;
    }
#endif
    if (! fd_handled) {
        close(acquire_fd);
    }

    waywallen_frame_t frame    = { 0 };
    frame.buffer_index         = fr.buffer_index;
    frame.seq                  = fr.seq;
    frame.vk_acquire_semaphore = acquire_semaphore;
    /* Hand the raw release_syncobj fd to the host. Ownership transfers:
     * the host MUST signal it from its release GPU work and then close.
     * DMABUF_RELAY mode sets this to -1: the lib already signaled. */
    frame.release_syncobj_fd = release_syncobj_fd;
    frame.buffer_generation  = fr.buffer_generation;
    if (release_armed) {
        if (acknowledge_frame_release(d, fr.buffer_generation, fr.seq) != WAYWALLEN_OK) {
            ww_evt_frame_ready_free(&fr);
            return WAYWALLEN_ERR_IO;
        }
    }
    ww_evt_frame_ready_free(&fr);

    if (d->cb.on_frame_ready) {
        d->cb.on_frame_ready(d->cb.user_data, &frame);
    } else if (release_syncobj_fd >= 0) {
        /* No callback means the host never reads this frame. */
        int rc = resolve_frame_without_gpu(d,
                                           release_syncobj_fd,
                                           frame.buffer_generation,
                                           frame.seq,
                                           "uncallbacked frame release failed");
        if (rc != WAYWALLEN_OK) return rc;
    }
    return WAYWALLEN_OK;
}

static int handle_unbind(waywallen_display_t* d, const uint8_t* body, size_t body_len) {
    ww_evt_unbind_t ub;
    if (ww_evt_unbind_decode(body, body_len, &ub) != WW_OK) {
        fire_disconnected(d, WAYWALLEN_ERR_PROTO, "decode unbind");
        return WAYWALLEN_ERR_PROTO;
    }
    uint64_t buffer_generation = ub.buffer_generation;
    ww_evt_unbind_free(&ub);
    bool failed_generation =
        d->has_failed_buffer_generation && buffer_generation == d->failed_buffer_generation;
    if (! failed_generation && (! d->bound.valid || buffer_generation != d->bound.generation)) {
        fire_disconnected(d, WAYWALLEN_ERR_PROTO, "unbind generation mismatch");
        return WAYWALLEN_ERR_PROTO;
    }
    if (failed_generation) {
        d->has_failed_buffer_generation = false;
    } else {
        fire_textures_releasing_if_any(d);
    }
    /* This acknowledges ownership transfer to the deferred-destroy
     * queue. GPU completion remains owned by each release syncobj. */
    ww_req_ack_unbind_t ack = { .buffer_generation = buffer_generation };
    int rc = outbox_enqueue_request(d, WW_OUTBOX_CRITICAL, WW_REQ_ACK_UNBIND, enc_ack_unbind, &ack);
    if (rc != WAYWALLEN_OK) {
        fire_disconnected(d, rc, "ack_unbind enqueue failed");
        return rc;
    }
    return WAYWALLEN_OK;
}

static int handle_error(waywallen_display_t* d, const uint8_t* body, size_t body_len) {
    ww_evt_error_t er;
    if (ww_evt_error_decode(body, body_len, &er) != WW_OK) {
        fire_disconnected_r(
            d, WAYWALLEN_DISCONNECT_DAEMON_ERROR, WAYWALLEN_ERR_PROTO, "decode error");
        return WAYWALLEN_ERR_PROTO;
    }
    /* Make a local copy of the message before freeing `er`, then
     * fire the callback. */
    char*    msg  = er.message;
    uint32_t code = er.code;
    er.message    = NULL;
    ww_evt_error_free(&er);
    fire_disconnected_r(
        d, map_daemon_error_code(code), WAYWALLEN_ERR_PROTO, msg ? msg : "server error");
    free(msg);
    return WAYWALLEN_ERR_PROTO;
}

int waywallen_display_dispatch(waywallen_display_t* d) {
    if (! d) return WAYWALLEN_ERR_INVAL;
    if (d->fd < 0 || d->conn == WW_CONN_DEAD) return WAYWALLEN_ERR_NOTCONN;
    if (d->conn == WW_CONN_DISCONNECTED) return WAYWALLEN_ERR_STATE;

    /* Stateful non-blocking read, same path as the handshake. The
     * fd is SOCK_NONBLOCK, so a partial frame (split header/body, or a
     * large frame like bind_buffers split across reads) must return
     * FRAME_NEED and resume on the next POLLIN — NOT be treated as a
     * fatal IO error. The earlier one-shot blocking codec turned every
     * such partial read into a spurious disconnect → respawn storm. */
    int rc = ww_codec_recv_partial(d->fd, &d->hs_recv);
    if (rc == WW_CODEC_FRAME_NEED)
        return WAYWALLEN_OK; /* 0 frames this round; level-triggered IN refires */
    if (rc < 0) {
        int werr = (rc == -ECONNRESET) ? WAYWALLEN_ERR_NOTCONN : WAYWALLEN_ERR_IO;
        fire_disconnected(d, werr, "recv event");
        flush_dead_event(d);
        return werr;
    }

    uint16_t op       = d->hs_recv.op;
    uint8_t* body_buf = d->hs_recv.body;
    size_t   body_len = d->hs_recv.body_len;
    int*     fd_buf   = d->hs_recv.fds;
    size_t   n_fds    = d->hs_recv.n_fds;
    int      ret;

    switch (op) {
    case WW_EVT_BIND_BUFFERS:
        ret = handle_bind_buffers(d, body_buf, body_len, fd_buf, n_fds);
        break;
    case WW_EVT_SET_COMPOSITION_CONFIG:
        close_all_fds(fd_buf, n_fds);
        ret = handle_set_composition_config(d, body_buf, body_len);
        break;
    case WW_EVT_SET_PRESENTATION_SNAPSHOT:
        close_all_fds(fd_buf, n_fds);
        ret = handle_set_presentation_snapshot(d, body_buf, body_len);
        break;
    case WW_EVT_SET_PRESENTATION_STATE:
        close_all_fds(fd_buf, n_fds);
        ret = handle_set_presentation_state(d, body_buf, body_len);
        break;
    case WW_EVT_FRAME_READY: ret = handle_frame_ready(d, body_buf, body_len, fd_buf, n_fds); break;
    case WW_EVT_UNBIND:
        close_all_fds(fd_buf, n_fds);
        ret = handle_unbind(d, body_buf, body_len);
        break;
    case WW_EVT_ERROR:
        close_all_fds(fd_buf, n_fds);
        ret = handle_error(d, body_buf, body_len);
        break;
    case WW_EVT_WELCOME:
    case WW_EVT_DISPLAY_ACCEPTED:
        /* Legal only during handshake. Seeing them again is a
         * protocol violation. */
        close_all_fds(fd_buf, n_fds);
        fire_disconnected(d, WAYWALLEN_ERR_PROTO, "unexpected handshake event");
        ret = WAYWALLEN_ERR_PROTO;
        break;
    default:
        /* Unknown opcodes are forward-compat: log + drop. */
        close_all_fds(fd_buf, n_fds);
        ret = WAYWALLEN_OK;
        break;
    }
    /* Every switch branch consumes (imports or closes) the frame's fds,
     * so zero n_fds before reset — otherwise recv_state_reset's
     * close_all would double-close fds whose numbers have already been
     * reused (e.g. by the shadow dmabuf dup), corrupting live fds.
     * Reset must precede flush_dead_event, which may free d. */
    d->hs_recv.n_fds = 0;
    ww_codec_recv_state_reset(&d->hs_recv);
    flush_dead_event(d); /* must be last; may free d */
    return ret;
}

/* ------------------------------------------------------------------ */
/*  CPU-side release_syncobj signal helper                            */
/* ------------------------------------------------------------------ */

/* Minimal redefinitions of the kernel drm_syncobj uAPI so we don't
 * pull <libdrm/drm.h> or <drm/drm.h> as a build dependency on every
 * consumer host. These match the layouts in <linux/drm.h>. */
struct ww_drm_syncobj_handle {
    uint32_t handle;
    uint32_t flags;
    int32_t  fd;
    uint32_t pad;
};
struct ww_drm_syncobj_create {
    uint32_t handle;
    uint32_t flags;
};
struct ww_drm_syncobj_destroy {
    uint32_t handle;
    uint32_t pad;
};
struct ww_drm_syncobj_transfer {
    uint32_t src_handle;
    uint32_t dst_handle;
    uint64_t src_point;
    uint64_t dst_point;
    uint32_t flags;
    uint32_t pad;
};
struct ww_drm_syncobj_array {
    uint64_t handles;
    uint32_t count_handles;
    uint32_t pad;
};

#ifndef DRM_IOCTL_BASE
#    define DRM_IOCTL_BASE 'd'
#endif
#define WW_DRM_IOCTL_SYNCOBJ_CREATE       _IOWR(DRM_IOCTL_BASE, 0xBF, struct ww_drm_syncobj_create)
#define WW_DRM_IOCTL_SYNCOBJ_DESTROY      _IOWR(DRM_IOCTL_BASE, 0xC0, struct ww_drm_syncobj_destroy)
#define WW_DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE _IOWR(DRM_IOCTL_BASE, 0xC2, struct ww_drm_syncobj_handle)
#define WW_DRM_IOCTL_SYNCOBJ_SIGNAL       _IOWR(DRM_IOCTL_BASE, 0xC5, struct ww_drm_syncobj_array)
#define WW_DRM_IOCTL_SYNCOBJ_TRANSFER _IOWR(DRM_IOCTL_BASE, 0xCC, struct ww_drm_syncobj_transfer)

#define WW_DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE (1u << 0)

/* Cached render-node fd. Opened lazily on first call; never closed
 * (process-lifetime). The kernel allows many concurrent open()s and
 * the file is small. */
static int ww_drm_node_fd(void) {
    static int cached = -1;
    if (cached >= 0) return cached;
    for (int minor = 128; minor <= 192; ++minor) {
        char path[64];
        if (snprintf(path, sizeof(path), "/dev/dri/renderD%d", minor) <= 0) continue;
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd >= 0) {
            cached = fd;
            return cached;
        }
    }
    return -1;
}

int waywallen_display_signal_release_syncobj(int fd) {
    if (fd < 0) return WAYWALLEN_ERR_INVAL;
    int drm_fd = ww_drm_node_fd();
    if (drm_fd < 0) {
        close(fd);
        return WAYWALLEN_ERR_IO;
    }
    /* Import fd → handle on this process's DRM device. */
    struct ww_drm_syncobj_handle imp = {
        .handle = 0,
        .flags  = 0,
        .fd     = fd,
        .pad    = 0,
    };
    if (ioctl(drm_fd, WW_DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &imp) != 0) {
        close(fd);
        return WAYWALLEN_ERR_IO;
    }
    int rc = WAYWALLEN_OK;
    /* Signal the handle. */
    uint32_t                    handles[1] = { imp.handle };
    struct ww_drm_syncobj_array sig        = {
        .handles       = (uintptr_t)handles,
        .count_handles = 1,
        .pad           = 0,
    };
    if (ioctl(drm_fd, WW_DRM_IOCTL_SYNCOBJ_SIGNAL, &sig) != 0) {
        rc = WAYWALLEN_ERR_IO;
    }
    /* Drop the handle on this device's side. The kernel keeps the
     * syncobj alive as long as any other fd or handle still refs it
     * (the daemon's handle, in our case — that's what the reaper
     * waits on). */
    struct ww_drm_syncobj_destroy dst = { .handle = imp.handle, .pad = 0 };
    (void)ioctl(drm_fd, WW_DRM_IOCTL_SYNCOBJ_DESTROY, &dst);
    close(fd);
    return rc;
}

int waywallen_display_release_after_sync_file(int release_syncobj_fd, int sync_file_fd) {
    if (release_syncobj_fd < 0 || sync_file_fd < 0) {
        if (release_syncobj_fd >= 0) close(release_syncobj_fd);
        if (sync_file_fd >= 0) close(sync_file_fd);
        return WAYWALLEN_ERR_INVAL;
    }
    int drm_fd = ww_drm_node_fd();
    if (drm_fd < 0) {
        close(release_syncobj_fd);
        close(sync_file_fd);
        return WAYWALLEN_ERR_IO;
    }

    int                          rc      = WAYWALLEN_ERR_IO;
    struct ww_drm_syncobj_handle release = {
        .handle = 0,
        .flags  = 0,
        .fd     = release_syncobj_fd,
        .pad    = 0,
    };
    if (ioctl(drm_fd, WW_DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &release) != 0) goto done;

    struct ww_drm_syncobj_create source = { 0 };
    if (ioctl(drm_fd, WW_DRM_IOCTL_SYNCOBJ_CREATE, &source) != 0) goto destroy_release;

    struct ww_drm_syncobj_handle import = {
        .handle = source.handle,
        .flags  = WW_DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE,
        .fd     = sync_file_fd,
        .pad    = 0,
    };
    if (ioctl(drm_fd, WW_DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &import) == 0) {
        struct ww_drm_syncobj_transfer transfer = {
            .src_handle = source.handle,
            .dst_handle = release.handle,
            .src_point  = 0,
            .dst_point  = 0,
            .flags      = 0,
            .pad        = 0,
        };
        if (ioctl(drm_fd, WW_DRM_IOCTL_SYNCOBJ_TRANSFER, &transfer) == 0) {
            rc = WAYWALLEN_OK;
        }
    }

    {
        struct ww_drm_syncobj_destroy destroy = { .handle = source.handle, .pad = 0 };
        (void)ioctl(drm_fd, WW_DRM_IOCTL_SYNCOBJ_DESTROY, &destroy);
    }
destroy_release: {
    struct ww_drm_syncobj_destroy destroy = { .handle = release.handle, .pad = 0 };
    (void)ioctl(drm_fd, WW_DRM_IOCTL_SYNCOBJ_DESTROY, &destroy);
}
done:
    close(release_syncobj_fd);
    close(sync_file_fd);
    return rc;
}

void waywallen_display_close(waywallen_display_t* d) {
    if (! d) return;
    if (d->fd >= 0) {
        close(d->fd);
        d->fd = -1;
    }
    fire_textures_releasing_if_any(d);
#ifdef WW_HAVE_VULKAN
    if (d->backend == WAYWALLEN_BACKEND_VULKAN && ww_vk_blitter_initialized(&d->vk_blitter)) {
        ww_vk_blitter_shutdown(&d->vk_blitter);
    }
    /* DMABUF_RELAY owns the complete Vulkan stack. */
    if (d->backend == WAYWALLEN_BACKEND_DMABUF_RELAY) {
        ww_vk_blitter_shutdown(&d->vk_blitter);
        ww_vk_backend_unload(&d->vk_backend);
        ww_vk_destroy_owned(&d->vk_owned);
    }
#endif
    d->conn             = WW_CONN_DISCONNECTED;
    d->bound.phase      = WW_STREAM_IDLE;
    d->hs_state         = WW_HS_IDLE;
    d->presentation     = presentation_reset_snapshot();
    d->has_presentation = false;
    ww_codec_recv_state_reset(&d->hs_recv);
    d->out_len = 0;
    d->out_pos = 0;
    outbox_reset_queue(d);
    /* Drop any latched dead-event — host called us, we don't want to
     * fire on_disconnected back at them. */
    d->dead_event_pending = false;
    d->dead_err           = 0;
}

void waywallen_display_shutdown(waywallen_display_t* d) {
    if (! d) return;
    waywallen_display_close(d);
    /* Drain in a bounded loop. Each iteration destroys whatever pools
     * the previous bind_buffers handler queued; in practice 1 pass is
     * always enough, but a few extra absorb late-binding edge cases. */
    for (int i = 0; i < 4; i++) {
        if (waywallen_display_drain(d) <= 0) {
            waywallen_display_free(d);
            return;
        }
    }
    /* Drain still has work after 4 iterations — the host's render
     * thread context is wedged or someone is concurrently pushing
     * pools. free's abort will catch it. */
    waywallen_display_free(d);
}

/* ------------------------------------------------------------------ */
/*  State queries                                                      */
/* ------------------------------------------------------------------ */

waywallen_conn_state_t waywallen_display_conn_state(waywallen_display_t* d) {
    if (! d) return WAYWALLEN_CONN_DISCONNECTED;
    return (waywallen_conn_state_t)d->conn;
}

waywallen_stream_state_t waywallen_display_stream_state(waywallen_display_t* d) {
    if (! d) return WAYWALLEN_STREAM_INACTIVE;
    return d->bound.phase == WW_STREAM_IDLE ? WAYWALLEN_STREAM_INACTIVE : WAYWALLEN_STREAM_ACTIVE;
}

uint64_t waywallen_display_get_display_id(waywallen_display_t* d) {
    if (! d) return 0;
    return d->display_id;
}

int waywallen_display_get_presentation_snapshot(
    waywallen_display_t* d, waywallen_presentation_snapshot_t* out_presentation) {
    if (! d || ! out_presentation) return WAYWALLEN_ERR_INVAL;
    if (! d->has_presentation) return WAYWALLEN_ERR_NOTCONN;
    *out_presentation = d->presentation;
    return WAYWALLEN_OK;
}

waywallen_disconnect_reason_t waywallen_display_last_disconnect_reason(waywallen_display_t* d) {
    if (! d) return WAYWALLEN_DISCONNECT_NONE;
    return d->last_reason;
}

const char* waywallen_display_last_disconnect_message(waywallen_display_t* d) {
    if (! d) return "";
    return d->last_message;
}

/* ------------------------------------------------------------------ */
/*  EGL deferred GL texture creation                                   */
/* ------------------------------------------------------------------ */

int waywallen_display_create_gl_texture(waywallen_display_t* d, uint32_t idx,
                                        uint32_t* out_gl_texture) {
#ifdef WW_HAVE_EGL
    if (! d || ! out_gl_texture) return WAYWALLEN_ERR_INVAL;
    if (d->backend != WAYWALLEN_BACKEND_EGL || ! d->egl_backend.loaded) return WAYWALLEN_ERR_STATE;
    if (idx >= d->egl_import_count || ! d->egl_images) return WAYWALLEN_ERR_INVAL;
    if (! d->egl_images[idx]) return WAYWALLEN_ERR_INVAL;

    /* Already created? Return the cached texture. */
    if (d->egl_gl_textures && d->egl_gl_textures[idx]) {
        *out_gl_texture = d->egl_gl_textures[idx];
        return WAYWALLEN_OK;
    }

    GLuint tex = 0;
    int    rc  = ww_egl_texture_from_image(&d->egl_backend, (EGLImageKHR)d->egl_images[idx], &tex);
    if (rc != 0) {
        ww_log(WAYWALLEN_LOG_WARN, "GL texture creation failed for image %u: %d", idx, rc);
        return WAYWALLEN_ERR_IO;
    }

    ww_log(WAYWALLEN_LOG_DEBUG, "created GL texture %u for image %u", tex, idx);
    d->egl_gl_textures[idx] = tex;
    *out_gl_texture         = tex;
    return WAYWALLEN_OK;
#else
    (void)d;
    (void)idx;
    (void)out_gl_texture;
    return WAYWALLEN_ERR_NOT_IMPL;
#endif
}

void waywallen_display_delete_gl_texture(waywallen_display_t* d, uint32_t idx) {
#ifdef WW_HAVE_EGL
    if (! d || d->backend != WAYWALLEN_BACKEND_EGL) return;
    if (! d->egl_backend.loaded) return;
    if (idx >= d->egl_import_count) return;
    if (! d->egl_gl_textures || ! d->egl_gl_textures[idx]) return;

    d->egl_backend.glDeleteTextures(1, &d->egl_gl_textures[idx]);
    d->egl_gl_textures[idx] = 0;
#else
    (void)d;
    (void)idx;
#endif
}
