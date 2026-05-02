/*
 * libwaywallen_display — lifecycle + state machine.
 *
 * Phase 2: full wire handshake + dispatch loop + release path.
 * Backend bindings are recorded but no texture import is performed;
 * `on_textures_ready` fires with `backend = NONE` and NULL handle
 * arrays. Incoming dma-buf fds and acquire sync_fds are closed by
 * the library without being surfaced to the host. All protocol /
 * IO errors transition the state machine to DEAD and invoke
 * `on_disconnected`. No code path aborts or exits.
 */

#define _POSIX_C_SOURCE 200809L

#include "waywallen_display.h"

#include "codec.h"
#include "ww_proto.h"

#ifdef WW_HAVE_EGL
#  include "backend_egl.h"
#endif
#ifdef WW_HAVE_VULKAN
#  include "backend_vulkan.h"
#endif

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
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
/*  Logging                                                            */
/* ------------------------------------------------------------------ */

static waywallen_log_callback_t s_log_cb = NULL;
static void *s_log_ud = NULL;

void waywallen_display_set_log_callback(waywallen_log_callback_t cb,
                                        void *user_data) {
    s_log_cb = cb;
    s_log_ud = user_data;
}

__attribute__((format(printf, 2, 3), visibility("hidden")))
void ww_log(waywallen_log_level_t level, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (s_log_cb) {
        s_log_cb(level, buf, s_log_ud);
    } else {
        static const char *tags[] = { "DEBUG", "INFO", "WARN", "ERROR" };
        fprintf(stderr, "waywallen_display [%s] %s\n",
                tags[level < 4 ? level : 3], buf);
    }
}

/* Internal connection state (maps to public waywallen_conn_state_t). */
typedef enum ww_conn_state {
    WW_CONN_DISCONNECTED = 0,
    WW_CONN_CONNECTING,
    WW_CONN_CONNECTED,
    WW_CONN_DEAD,
} ww_conn_state_t;

/* Internal stream state (maps to public waywallen_stream_state_t). */
typedef enum ww_stream_state {
    WW_STREAM_INACTIVE = 0,
    WW_STREAM_ACTIVE,
} ww_stream_state_t;

/* Internal handshake state (maps to public waywallen_handshake_state_t).
 * Numerically aligned with the public enum so the accessor is a cast. */
typedef enum ww_handshake_state {
    WW_HS_IDLE          = 0,
    WW_HS_CONNECTING    = 1,
    WW_HS_HELLO_PENDING = 2,
    WW_HS_WELCOME_WAIT  = 3,
    WW_HS_REGISTER_PEND = 4,
    WW_HS_ACCEPTED_WAIT = 5,
    WW_HS_READY         = 6,
} ww_handshake_state_t;

/* Sized to comfortably hold the largest plausible handshake message
 * (hello: 3 short strings; register_display: name string + small
 * scalars + empty kv_list). 1 KiB leaves headroom without bloating
 * every display instance. */
#define WW_HS_SEND_BUF_BYTES 1024

struct waywallen_display {
    waywallen_display_callbacks_t cb;

    /* Connection to the backend daemon. */
    int fd;
    ww_conn_state_t conn;
    uint64_t display_id;

    /* Whether the backend is actively pushing frames. */
    ww_stream_state_t stream;

    /* Handshake state machine. Only meaningful while conn is
     * CONNECTING; reset to IDLE on disconnect/dead. */
    ww_handshake_state_t hs_state;
    uint8_t  hs_send_buf[WW_HS_SEND_BUF_BYTES];
    size_t   hs_send_len;       /* total bytes queued (header + body) */
    size_t   hs_send_pos;       /* bytes flushed so far */
    ww_codec_recv_state_t hs_recv;
    /* Saved register_display params, captured in begin_connect and
     * applied when WELCOME_WAIT transitions to REGISTER_PEND. */
    char     hs_display_name[256];
    /* Stable per-(DE,screen) identifier used by the daemon as the key
     * into per-display settings. Empty string means "no stable id";
     * the daemon then falls back to keying by `hs_display_name`. */
    char     hs_instance_id[128];
    uint32_t hs_display_width;
    uint32_t hs_display_height;
    uint32_t hs_display_refresh_mhz;
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

    /* Backend selection. */
    waywallen_backend_t backend;
    waywallen_egl_ctx_t egl;
    waywallen_vk_ctx_t vk;

#ifdef WW_HAVE_EGL
    /* Populated on `bind_egl` if libEGL is loadable. */
    ww_egl_backend_t egl_backend;
    uint32_t egl_import_count;
    void **egl_images;
    uint32_t *egl_gl_textures;
#endif
#ifdef WW_HAVE_VULKAN
    ww_vk_backend_t vk_backend;
    uint32_t vk_import_count;
    ww_vk_imported_image_t *vk_images;   /* length = vk_import_count */
    VkSemaphore *vk_semaphores;          /* one per buffer slot */
#endif

    /* Current bound buffer pool metadata — kept so
     * `on_textures_releasing` can fire with the same descriptor when
     * unbind/rebind arrives. */
    bool has_textures;
    uint64_t current_buffer_generation;
    waywallen_textures_t current_textures;
};

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

static void fire_disconnected(waywallen_display_t *d, int err, const char *msg) {
    if (d->conn == WW_CONN_DEAD) return;
    d->conn = WW_CONN_DEAD;
    d->stream = WW_STREAM_INACTIVE;
    d->hs_state = WW_HS_IDLE;
    ww_codec_recv_state_reset(&d->hs_recv);
    d->hs_send_len = 0;
    d->hs_send_pos = 0;
    if (d->cb.on_disconnected) {
        d->cb.on_disconnected(d->cb.user_data, err, msg);
    }
    if (d->fd >= 0) {
        close(d->fd);
        d->fd = -1;
    }
}

static void close_all_fds(int *fds, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (fds[i] >= 0) close(fds[i]);
    }
}

static int default_socket_path(char *out, size_t cap) {
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    const char *base = (runtime && runtime[0]) ? runtime : "/tmp";
    int n = snprintf(out, cap, "%s/waywallen/display.sock", base);
    if (n < 0 || (size_t)n >= cap) return WAYWALLEN_ERR_INVAL;
    return WAYWALLEN_OK;
}

/* Blocking helper: encode a request body into a scratch buf, hand it
 * to the codec, free the buf. */
static int send_request_msg(waywallen_display_t *d, uint16_t opcode,
                            int (*encode)(const void *, ww_buf_t *),
                            const void *msg) {
    ww_buf_t body;
    ww_buf_init(&body);
    int rc = encode(msg, &body);
    if (rc != WW_OK) {
        ww_buf_free(&body);
        return WAYWALLEN_ERR_NOMEM;
    }
    rc = ww_codec_send_request(d->fd, opcode, body.data, body.len, NULL, 0);
    ww_buf_free(&body);
    if (rc < 0) return WAYWALLEN_ERR_IO;
    return WAYWALLEN_OK;
}

/* Encoder trampolines with void-pointer signature so send_request_msg
 * can call them generically. */
static int enc_hello(const void *m, ww_buf_t *out) {
    return ww_req_hello_encode((const ww_req_hello_t *)m, out);
}
static int enc_register(const void *m, ww_buf_t *out) {
    return ww_req_register_display_encode((const ww_req_register_display_t *)m, out);
}
static int enc_update(const void *m, ww_buf_t *out) {
    return ww_req_update_display_encode((const ww_req_update_display_t *)m, out);
}
/* enc_release was removed when v1 dropped the BufferRelease request.
 * Release is now signaled by the host on the per-frame
 * release_syncobj fd that arrives with frame_ready. */
static int enc_consumer_caps(const void *m, ww_buf_t *out) {
    return ww_req_consumer_caps_encode((const ww_req_consumer_caps_t *)m, out);
}

/* Modifier-negotiation v2 — bit constants. Mirrored from
 * waywallen/src/negotiate.rs; keep in sync.
 *
 * The daemon advertises "modifier_negotiation_v1" in welcome.features
 * to indicate it understands these messages. */
#define WW_USAGE_SAMPLED          (1u << 0)
#define WW_MEM_HINT_DEVICE_LOCAL  (1u << 0)
#define WW_MEM_HINT_HOST_VISIBLE  (1u << 1)
#define WW_MEM_HINT_LINEAR_ONLY   (1u << 4)
#define WW_SYNC_SYNCOBJ_BINARY    (1u << 1)
#define WW_SYNC_SYNCOBJ_TIMELINE  (1u << 2)
#define WW_COLOR_ENC_SRGB         (1u << 0)
#define WW_COLOR_RANGE_LIMITED    (1u << 6)
#define WW_COLOR_ALPHA_PREMUL     (1u << 7)
#define WW_DRM_FORMAT_ABGR8888    0x34324241u
#define WW_DRM_FORMAT_XRGB8888    0x34325258u
#define WW_DRM_FORMAT_MOD_LINEAR  0ULL

/* Per-(fourcc, modifier) accumulator used while probing the active
 * backend. Grows by doubling. */
typedef struct ww_caps_buf {
    uint32_t *fourccs;       /* one entry per modifier (flattened) */
    uint64_t *modifiers;
    uint32_t *plane_counts;
    uint32_t *usages;
    size_t    n;
    size_t    cap;
    int       oom;
} ww_caps_buf_t;

static int ww_caps_buf_grow(ww_caps_buf_t *b) {
    size_t n = b->cap ? b->cap * 2 : 16;
    uint32_t *f = (uint32_t *)realloc(b->fourccs, n * sizeof(*f));
    uint64_t *m = (uint64_t *)realloc(b->modifiers, n * sizeof(*m));
    uint32_t *p = (uint32_t *)realloc(b->plane_counts, n * sizeof(*p));
    uint32_t *u = (uint32_t *)realloc(b->usages, n * sizeof(*u));
    if (!f || !m || !p || !u) {
        free(f); free(m); free(p); free(u);
        b->oom = 1;
        return -ENOMEM;
    }
    b->fourccs = f; b->modifiers = m; b->plane_counts = p; b->usages = u;
    b->cap = n;
    return 0;
}

static void ww_caps_buf_emit(uint32_t fourcc, uint64_t modifier,
                             uint32_t plane_count, uint32_t usage,
                             void *user_data) {
    ww_caps_buf_t *b = (ww_caps_buf_t *)user_data;
    if (b->oom) return;
    if (b->n >= b->cap && ww_caps_buf_grow(b) != 0) return;
    b->fourccs[b->n]      = fourcc;
    b->modifiers[b->n]    = modifier;
    b->plane_counts[b->n] = plane_count;
    b->usages[b->n]       = usage;
    b->n++;
}

static void ww_caps_buf_free(ww_caps_buf_t *b) {
    free(b->fourccs);
    free(b->modifiers);
    free(b->plane_counts);
    free(b->usages);
    memset(b, 0, sizeof(*b));
}

/* Group the flattened (fourcc, modifier) pairs into the wire encoding
 * the protocol expects: distinct fourccs, with mod_counts[i] giving
 * the number of modifiers attached to fourccs[i]. Stable iteration
 * order: first occurrence of a fourcc wins. Returns 0 on success.
 * Allocates `*out_fourccs` / `*out_mod_counts`; caller frees. */
static int ww_caps_group_fourccs(const ww_caps_buf_t *flat,
                                 uint32_t **out_fourccs,
                                 uint32_t **out_mod_counts,
                                 size_t *out_n_fourccs) {
    uint32_t *fourccs = (uint32_t *)calloc(flat->n, sizeof(*fourccs));
    uint32_t *counts  = (uint32_t *)calloc(flat->n, sizeof(*counts));
    if (!fourccs || !counts) {
        free(fourccs); free(counts);
        return -ENOMEM;
    }
    size_t k = 0;
    for (size_t i = 0; i < flat->n; ++i) {
        uint32_t f = flat->fourccs[i];
        size_t found = SIZE_MAX;
        for (size_t j = 0; j < k; ++j) {
            if (fourccs[j] == f) { found = j; break; }
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

/* Build + send a `consumer_caps` request to the daemon. Called from
 * the handshake state machine right after `display_accepted` and
 * before transitioning to READY. Synchronous; uses the same blocking
 * codec path as `send_request_msg`.
 *
 * Probes the active backend (EGL or Vulkan) for its real
 * (fourcc, modifier) import set + device UUID. Falls back to a
 * hardcoded ABGR/XRGB + LINEAR set with zero UUIDs only if the
 * probe fails or no backend is bound (the daemon's picker then
 * forces HOST_VISIBLE and treats this peer as cross-GPU). */
static int send_consumer_caps_blocking(waywallen_display_t *d) {
    ww_caps_buf_t buf = {0};
    uint8_t dev_uuid_bytes[16] = {0};
    uint8_t drv_uuid_bytes[16] = {0};
    bool advertise_device_local = false;

    int probe_rc = -ENOSYS;
    switch (d->backend) {
#ifdef WW_HAVE_EGL
    case WAYWALLEN_BACKEND_EGL:
        if (d->egl_backend.loaded) {
            probe_rc = ww_egl_query_format_caps(
                &d->egl_backend, (EGLDisplay)d->egl.egl_display,
                ww_caps_buf_emit, &buf);
            /* No portable EGL/GBM way to ask "do you have DEVICE_LOCAL
             * memory I can import a dmabuf into?" — GBM consumers
             * effectively always end up in GTT (HOST_VISIBLE). Leave
             * advertise_device_local=false. */
        }
        break;
#endif
#ifdef WW_HAVE_VULKAN
    case WAYWALLEN_BACKEND_VULKAN:
        if (d->vk_backend.loaded) {
            probe_rc = ww_vk_query_format_caps(
                &d->vk_backend, ww_caps_buf_emit, &buf);
            if (probe_rc == 0) {
                /* Best-effort UUIDs; ignore failure → leave zeros. */
                (void)ww_vk_query_device_uuid(
                    &d->vk_backend, dev_uuid_bytes, drv_uuid_bytes);
                /* If the device exposes any DEVICE_LOCAL memory type,
                 * advertise the bit so the daemon's same-device
                 * intersection (`pick_mem_hint`) can land on
                 * DEVICE_LOCAL when the producer also asks. The actual
                 * dmabuf import still re-validates memoryTypeBits at
                 * `vkAllocateMemory` time, so this hint only widens
                 * the daemon's choice — it can't force a bad mapping. */
                int has_dl = 0;
                (void)ww_vk_query_supports_device_local(
                    &d->vk_backend, &has_dl);
                advertise_device_local = (has_dl != 0);
            }
        }
        break;
#endif
    default:
        break;
    }
    if (probe_rc != 0 || buf.n == 0 || buf.oom) {
        ww_caps_buf_free(&buf);
        ww_log(WAYWALLEN_LOG_INFO,
               "consumer_caps: backend probe unavailable (rc=%d, n=%zu); "
               "falling back to ABGR/XRGB + LINEAR",
               probe_rc, buf.n);
        ww_caps_buf_emit(WW_DRM_FORMAT_ABGR8888, WW_DRM_FORMAT_MOD_LINEAR,
                         1, WW_USAGE_SAMPLED, &buf);
        ww_caps_buf_emit(WW_DRM_FORMAT_XRGB8888, WW_DRM_FORMAT_MOD_LINEAR,
                         1, WW_USAGE_SAMPLED, &buf);
    } else {
        ww_log(WAYWALLEN_LOG_INFO,
               "consumer_caps: backend probe yielded %zu (fourcc, modifier) entries",
               buf.n);
    }
    if (buf.oom) {
        ww_caps_buf_free(&buf);
        return -ENOMEM;
    }

    uint32_t *grp_fourccs = NULL;
    uint32_t *grp_counts  = NULL;
    size_t    grp_n       = 0;
    int gr = ww_caps_group_fourccs(&buf, &grp_fourccs, &grp_counts, &grp_n);
    if (gr != 0) {
        ww_caps_buf_free(&buf);
        return gr;
    }

    /* Pack the 16-byte UUIDs as 4×u32 little-endian for the wire. */
    uint32_t dev_uuid_w[4];
    uint32_t drv_uuid_w[4];
    for (int i = 0; i < 4; ++i) {
        memcpy(&dev_uuid_w[i], dev_uuid_bytes + i * 4, 4);
        memcpy(&drv_uuid_w[i], drv_uuid_bytes + i * 4, 4);
    }

    /* LINEAR_ONLY: every advertised modifier is DRM_FORMAT_MOD_LINEAR.
     * Covers both the probe-failed fallback (we just emitted ABGR/XRGB
     * + LINEAR above) and the case where the driver only reports
     * LINEAR for the formats it advertises. Telling the daemon
     * up-front saves a `bind_failed` round-trip when the producer
     * happens to live on the same vendor and would otherwise have
     * tried a tile modifier. */
    bool linear_only = (buf.n > 0);
    for (size_t i = 0; i < buf.n; ++i) {
        if (buf.modifiers[i] != WW_DRM_FORMAT_MOD_LINEAR) {
            linear_only = false;
            break;
        }
    }

    ww_req_consumer_caps_t m;
    memset(&m, 0, sizeof(m));
    m.fourccs.count       = (uint32_t)grp_n;
    m.fourccs.data        = grp_fourccs;
    m.mod_counts.count    = (uint32_t)grp_n;
    m.mod_counts.data     = grp_counts;
    m.modifiers.count     = (uint32_t)buf.n;
    m.modifiers.data      = buf.modifiers;
    m.usages.count        = (uint32_t)buf.n;
    m.usages.data         = buf.usages;
    m.plane_counts.count  = (uint32_t)buf.n;
    m.plane_counts.data   = buf.plane_counts;
    m.device_uuid.count   = 4;
    m.device_uuid.data    = dev_uuid_w;
    m.driver_uuid.count   = 4;
    m.driver_uuid.data    = drv_uuid_w;
    m.drm_render_major    = d->hs_drm_render_major;
    m.drm_render_minor    = d->hs_drm_render_minor;
    m.mem_hints           = WW_MEM_HINT_HOST_VISIBLE
                          | (advertise_device_local ? WW_MEM_HINT_DEVICE_LOCAL : 0u)
                          | (linear_only ? WW_MEM_HINT_LINEAR_ONLY : 0u);
    /* sync_caps: consumer-side release/wait always lands on the kernel
     * drm_syncobj ioctl path (`waywallen_display_signal_release_syncobj`
     * + `vk/egl_wait_sync_fd`); both BINARY and TIMELINE are always
     * supported regardless of which GPU API the host bound. No probe
     * needed. */
    m.sync_caps           = WW_SYNC_SYNCOBJ_TIMELINE | WW_SYNC_SYNCOBJ_BINARY;
    /* color_caps: encoding/range/alpha flags are interpretation-time
     * choices, not driver-locked capabilities — the consumer can
     * always sample any sRGB texture and apply any color transform
     * downstream. The defaults below are the most common desktop
     * compositor settings; daemon falls back to DEFAULT_COLOR per
     * axis if the producer's intersection is empty. No probe
     * needed. */
    m.color_caps          = WW_COLOR_ENC_SRGB | WW_COLOR_RANGE_LIMITED
                          | WW_COLOR_ALPHA_PREMUL;
    m.extent_max_w        = 16384;
    m.extent_max_h        = 16384;

    int rc = send_request_msg(d, WW_REQ_CONSUMER_CAPS, enc_consumer_caps, &m);
    free(grp_fourccs);
    free(grp_counts);
    ww_caps_buf_free(&buf);
    return rc;
}

static int enc_bye(const void *m, ww_buf_t *out) {
    return ww_req_bye_encode((const ww_req_bye_t *)m, out);
}

/* Receive the next event frame. `body_buf` must be ≥ WW_CODEC_MAX_BODY_BYTES.
 * Returns 0 on success and fills the out-params. */
static int recv_event_frame(waywallen_display_t *d,
                            uint16_t *op,
                            uint8_t *body_buf, size_t *body_len,
                            int *fd_buf, size_t fd_cap, size_t *n_fds) {
    int rc = ww_codec_recv_event(d->fd, op, body_buf, WW_CODEC_MAX_BODY_BYTES,
                                 body_len, fd_buf, fd_cap, n_fds);
    if (rc == 0) return WAYWALLEN_OK;
    if (rc == -ECONNRESET) return WAYWALLEN_ERR_NOTCONN;
    return WAYWALLEN_ERR_IO;
}

/* ------------------------------------------------------------------ */
/*  Lifecycle                                                          */
/* ------------------------------------------------------------------ */

waywallen_display_t *waywallen_display_new(const waywallen_display_callbacks_t *cb) {
    if (!cb) return NULL;
    waywallen_display_t *d = (waywallen_display_t *)calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->cb = *cb;
    d->fd = -1;
    d->conn = WW_CONN_DISCONNECTED;
    d->stream = WW_STREAM_INACTIVE;
    d->backend = WAYWALLEN_BACKEND_NONE;
    d->hs_state = WW_HS_IDLE;
    ww_codec_recv_state_init(&d->hs_recv);
    return d;
}

void waywallen_display_destroy(waywallen_display_t *d) {
    if (!d) return;
    if (d->fd >= 0) {
        close(d->fd);
        d->fd = -1;
    }
    ww_codec_recv_state_reset(&d->hs_recv);
    free(d);
}

int waywallen_display_bind_egl(waywallen_display_t *d,
                               const waywallen_egl_ctx_t *ctx) {
    if (!d || !ctx) return WAYWALLEN_ERR_INVAL;
    if (d->conn != WW_CONN_DISCONNECTED) return WAYWALLEN_ERR_STATE;
    d->backend = WAYWALLEN_BACKEND_EGL;
    d->egl = *ctx;
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
            int qrc = ww_egl_query_drm_render_node(
                &d->egl_backend, (EGLDisplay)ctx->egl_display, &major, &minor);
            if (qrc == 0) {
                d->hs_drm_render_major = major;
                d->hs_drm_render_minor = minor;
                ww_log(WAYWALLEN_LOG_INFO,
                       "egl drm render node = %u:%u", major, minor);
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

int waywallen_display_bind_vulkan(waywallen_display_t *d,
                                  const waywallen_vk_ctx_t *ctx) {
    if (!d || !ctx) return WAYWALLEN_ERR_INVAL;
    if (d->conn != WW_CONN_DISCONNECTED) return WAYWALLEN_ERR_STATE;
    /* vk_get_instance_proc_addr may be NULL: the backend will dlopen
     * libvulkan.so.1 and pull vkGetInstanceProcAddr from it directly. */
    d->backend = WAYWALLEN_BACKEND_VULKAN;
    d->vk = *ctx;
#ifdef WW_HAVE_VULKAN
    int rc = ww_vk_backend_load(
        &d->vk_backend,
        (VkInstance)ctx->instance,
        (VkPhysicalDevice)ctx->physical_device,
        (VkDevice)ctx->device,
        ctx->queue_family_index,
        (ww_vk_get_instance_proc_addr_fn)ctx->vk_get_instance_proc_addr);
    if (rc != 0) {
        ww_log(WAYWALLEN_LOG_WARN, "vk backend load failed: %d", rc);
        memset(&d->vk_backend, 0, sizeof(d->vk_backend));
    } else {
        ww_log(WAYWALLEN_LOG_INFO, "vk backend loaded");
        if (d->hs_drm_render_major == 0 && d->hs_drm_render_minor == 0) {
            uint32_t major = 0, minor = 0;
            int qrc = ww_vk_query_drm_render_node(
                &d->vk_backend, &major, &minor);
            if (qrc == 0) {
                d->hs_drm_render_major = major;
                d->hs_drm_render_minor = minor;
                ww_log(WAYWALLEN_LOG_INFO,
                       "vk drm render node = %u:%u", major, minor);
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

int waywallen_display_set_drm_render_node(waywallen_display_t *d,
                                          uint32_t major,
                                          uint32_t minor) {
    if (!d) return WAYWALLEN_ERR_INVAL;
    if (d->conn != WW_CONN_DISCONNECTED) return WAYWALLEN_ERR_STATE;
    d->hs_drm_render_major = major;
    d->hs_drm_render_minor = minor;
    return WAYWALLEN_OK;
}

/* ------------------------------------------------------------------ */
/*  Connect + handshake                                                */
/* ------------------------------------------------------------------ */

/* Open a UDS in non-blocking mode. *out_in_progress is true when
 * connect(2) returned EINPROGRESS (kernel will signal POLLOUT once
 * the connect completes). Returns fd on success, -errno on failure. */
static int open_uds_nonblock(const char *path, bool *out_in_progress) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return -errno;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    size_t pl = strlen(path);
    if (pl >= sizeof(addr.sun_path)) {
        close(fd);
        return -ENAMETOOLONG;
    }
    memcpy(addr.sun_path, path, pl);
    *out_in_progress = false;
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
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
 * (header + body) into d->hs_send_buf for the partial sender to drain. */
static int hs_queue_request(waywallen_display_t *d, uint16_t opcode,
                            int (*encode)(const void *, ww_buf_t *),
                            const void *msg) {
    ww_buf_t body;
    ww_buf_init(&body);
    int rc = encode(msg, &body);
    if (rc != WW_OK) {
        ww_buf_free(&body);
        return WAYWALLEN_ERR_NOMEM;
    }
    size_t total = 4 + body.len;
    if (total > sizeof(d->hs_send_buf)) {
        /* Should never happen for a handshake message. */
        ww_buf_free(&body);
        return WAYWALLEN_ERR_NOMEM;
    }
    d->hs_send_buf[0] = (uint8_t)(opcode & 0xff);
    d->hs_send_buf[1] = (uint8_t)((opcode >> 8) & 0xff);
    d->hs_send_buf[2] = (uint8_t)(total & 0xff);
    d->hs_send_buf[3] = (uint8_t)((total >> 8) & 0xff);
    if (body.len > 0) memcpy(d->hs_send_buf + 4, body.data, body.len);
    d->hs_send_len = total;
    d->hs_send_pos = 0;
    ww_buf_free(&body);
    return WAYWALLEN_OK;
}

static int hs_queue_hello(waywallen_display_t *d) {
    ww_req_hello_t hello;
    memset(&hello, 0, sizeof(hello));
    hello.protocol = (char *)WW_PROTOCOL_NAME;
    hello.client_name = (char *)"libwaywallen_display";
    hello.client_version = (char *)"0.1.0";
    hello.client_protocol_version = WAYWALLEN_DISPLAY_PROTOCOL_VERSION;
    return hs_queue_request(d, WW_REQ_HELLO, enc_hello, &hello);
}

static int hs_queue_register(waywallen_display_t *d) {
    ww_req_register_display_t reg;
    memset(&reg, 0, sizeof(reg));
    reg.name = d->hs_display_name;
    reg.instance_id = d->hs_instance_id;
    reg.width = d->hs_display_width;
    reg.height = d->hs_display_height;
    reg.refresh_mhz = d->hs_display_refresh_mhz;
    reg.drm_render_major = d->hs_drm_render_major;
    reg.drm_render_minor = d->hs_drm_render_minor;
    reg.properties.count = 0;
    reg.properties.data = NULL;
    return hs_queue_request(d, WW_REQ_REGISTER_DISPLAY, enc_register, &reg);
}

int waywallen_display_begin_connect_v2(waywallen_display_t *d,
                                       const char *socket_path,
                                       const char *display_name,
                                       const char *instance_id,
                                       uint32_t width,
                                       uint32_t height,
                                       uint32_t refresh_mhz) {
    if (!d || !display_name) return WAYWALLEN_ERR_INVAL;
    if (d->conn != WW_CONN_DISCONNECTED && d->conn != WW_CONN_DEAD) {
        return WAYWALLEN_ERR_STATE;
    }

    char path_buf[256];
    if (!socket_path) {
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
    d->hs_display_width = width;
    d->hs_display_height = height;
    d->hs_display_refresh_mhz = refresh_mhz;

    bool in_progress = false;
    int fd = open_uds_nonblock(socket_path, &in_progress);
    if (fd < 0) {
        return WAYWALLEN_ERR_IO;
    }
    d->fd = fd;
    d->conn = WW_CONN_CONNECTING;
    d->stream = WW_STREAM_INACTIVE;
    ww_codec_recv_state_reset(&d->hs_recv);
    d->hs_send_len = 0;
    d->hs_send_pos = 0;

    if (in_progress) {
        d->hs_state = WW_HS_CONNECTING;
    } else {
        /* connect(2) finished synchronously — queue hello so the very
         * first advance call can flush it. */
        int rc = hs_queue_hello(d);
        if (rc != WAYWALLEN_OK) {
            close(d->fd);
            d->fd = -1;
            d->conn = WW_CONN_DEAD;
            d->hs_state = WW_HS_IDLE;
            return rc;
        }
        d->hs_state = WW_HS_HELLO_PENDING;
    }
    return WAYWALLEN_OK;
}

int waywallen_display_begin_connect(waywallen_display_t *d,
                                    const char *socket_path,
                                    const char *display_name,
                                    uint32_t width,
                                    uint32_t height,
                                    uint32_t refresh_mhz) {
    return waywallen_display_begin_connect_v2(d, socket_path, display_name,
                                              NULL, width, height, refresh_mhz);
}

waywallen_handshake_state_t waywallen_display_handshake_state(waywallen_display_t *d) {
    if (!d) return WAYWALLEN_HS_IDLE;
    return (waywallen_handshake_state_t)d->hs_state;
}

/* Internal: advance one logical step. May return PROGRESS, in which
 * case advance_handshake() loops back into this function. */
static int hs_advance_one(waywallen_display_t *d) {
    switch (d->hs_state) {
    case WW_HS_IDLE:
    case WW_HS_READY:
        return WAYWALLEN_ERR_STATE;

    case WW_HS_CONNECTING: {
        int err = 0;
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
        ssize_t n = ww_codec_send_partial(d->fd,
                                          d->hs_send_buf + d->hs_send_pos,
                                          d->hs_send_len - d->hs_send_pos);
        if (n < 0) {
            fire_disconnected(d, WAYWALLEN_ERR_IO, "send handshake");
            return WAYWALLEN_ERR_IO;
        }
        if (n == 0) return WAYWALLEN_HS_NEED_WRITE;
        d->hs_send_pos += (size_t)n;
        if (d->hs_send_pos < d->hs_send_len) return WAYWALLEN_HS_NEED_WRITE;
        if (d->hs_state == WW_HS_HELLO_PENDING) {
            d->hs_state = WW_HS_WELCOME_WAIT;
        } else {
            d->hs_state = WW_HS_ACCEPTED_WAIT;
        }
        d->hs_send_len = 0;
        d->hs_send_pos = 0;
        return WAYWALLEN_HS_PROGRESS;
    }

    case WW_HS_WELCOME_WAIT: {
        int rc = ww_codec_recv_partial(d->fd, &d->hs_recv);
        if (rc == WW_CODEC_FRAME_NEED) return WAYWALLEN_HS_NEED_READ;
        if (rc < 0) {
            int werr = (rc == -ECONNRESET) ? WAYWALLEN_ERR_NOTCONN
                                            : WAYWALLEN_ERR_IO;
            fire_disconnected(d, werr, "recv welcome");
            return werr;
        }
        /* No fds expected on welcome; defensively close any. */
        close_all_fds(d->hs_recv.fds, d->hs_recv.n_fds);
        d->hs_recv.n_fds = 0;
        if (d->hs_recv.op == WW_EVT_ERROR) {
            ww_evt_error_t er;
            const char *msg = "server error";
            if (ww_evt_error_decode(d->hs_recv.body, d->hs_recv.body_len, &er) == WW_OK) {
                if (er.message) msg = er.message;
                fire_disconnected(d, WAYWALLEN_ERR_PROTO, msg);
                ww_evt_error_free(&er);
            } else {
                fire_disconnected(d, WAYWALLEN_ERR_PROTO,
                                  "server error (decode failed)");
            }
            ww_codec_recv_state_reset(&d->hs_recv);
            return WAYWALLEN_ERR_PROTO;
        }
        if (d->hs_recv.op != WW_EVT_WELCOME) {
            fire_disconnected(d, WAYWALLEN_ERR_PROTO, "expected welcome");
            ww_codec_recv_state_reset(&d->hs_recv);
            return WAYWALLEN_ERR_PROTO;
        }
        /* Decode purely for diagnostics — `welcome` is informational
         * in v3+. Version compatibility was already enforced by the
         * daemon before it sent welcome; reaching here means we are
         * a supported client. Features are advisory only. */
        ww_evt_welcome_t welcome;
        if (ww_evt_welcome_decode(d->hs_recv.body, d->hs_recv.body_len,
                                  &welcome) != WW_OK) {
            fire_disconnected(d, WAYWALLEN_ERR_PROTO, "decode welcome");
            ww_codec_recv_state_reset(&d->hs_recv);
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
            int werr = (rc == -ECONNRESET) ? WAYWALLEN_ERR_NOTCONN
                                            : WAYWALLEN_ERR_IO;
            fire_disconnected(d, werr, "recv display_accepted");
            return werr;
        }
        close_all_fds(d->hs_recv.fds, d->hs_recv.n_fds);
        d->hs_recv.n_fds = 0;
        if (d->hs_recv.op == WW_EVT_ERROR) {
            ww_evt_error_t er;
            const char *msg = "server error";
            if (ww_evt_error_decode(d->hs_recv.body, d->hs_recv.body_len, &er) == WW_OK) {
                if (er.message) msg = er.message;
                fire_disconnected(d, WAYWALLEN_ERR_PROTO, msg);
                ww_evt_error_free(&er);
            } else {
                fire_disconnected(d, WAYWALLEN_ERR_PROTO,
                                  "server error (decode failed)");
            }
            ww_codec_recv_state_reset(&d->hs_recv);
            return WAYWALLEN_ERR_PROTO;
        }
        if (d->hs_recv.op != WW_EVT_DISPLAY_ACCEPTED) {
            fire_disconnected(d, WAYWALLEN_ERR_PROTO, "expected display_accepted");
            ww_codec_recv_state_reset(&d->hs_recv);
            return WAYWALLEN_ERR_PROTO;
        }
        ww_evt_display_accepted_t accepted;
        if (ww_evt_display_accepted_decode(d->hs_recv.body,
                                           d->hs_recv.body_len,
                                           &accepted) != WW_OK) {
            fire_disconnected(d, WAYWALLEN_ERR_PROTO, "decode display_accepted");
            ww_codec_recv_state_reset(&d->hs_recv);
            return WAYWALLEN_ERR_PROTO;
        }
        d->display_id = accepted.display_id;
        ww_evt_display_accepted_free(&accepted);
        d->conn = WW_CONN_CONNECTED;
        // Modifier-negotiation v2 — ship a hardcoded LINEAR-only
        // ConsumerCaps before transitioning to READY. Real probing
        // (eglQueryDmaBufModifiersEXT / vkGetPhysicalDeviceFormatProperties2)
        // is a follow-up; LINEAR is the cross-vendor floor every backend
        // can import, so this is correct (if conservative). The
        // daemon's picker collapses to LINEAR cross-vendor anyway.
        if (send_consumer_caps_blocking(d) != WAYWALLEN_OK) {
            // Don't fail the handshake — the daemon falls back to
            // legacy behavior for displays without caps. The error is
            // surfaced via fire_disconnected if it was fatal.
        }
        d->hs_state = WW_HS_READY;
        ww_codec_recv_state_reset(&d->hs_recv);
        return WAYWALLEN_HS_DONE;
    }
    }
    return WAYWALLEN_ERR_STATE;
}

int waywallen_display_advance_handshake(waywallen_display_t *d) {
    if (!d) return WAYWALLEN_ERR_INVAL;
    if (d->conn != WW_CONN_CONNECTING) return WAYWALLEN_ERR_NOTCONN;
    /* Loop while the state machine reports PROGRESS so the caller only
     * sees terminal codes (DONE / NEED_* / error). Bounded by the
     * number of state transitions (≤4). */
    for (;;) {
        int rc = hs_advance_one(d);
        if (rc == WAYWALLEN_HS_PROGRESS) continue;
        return rc;
    }
}

int waywallen_display_connect_v2(waywallen_display_t *d,
                                 const char *socket_path,
                                 const char *display_name,
                                 const char *instance_id,
                                 uint32_t width,
                                 uint32_t height,
                                 uint32_t refresh_mhz) {
    int rc = waywallen_display_begin_connect_v2(d, socket_path, display_name,
                                                instance_id, width, height,
                                                refresh_mhz);
    if (rc != WAYWALLEN_OK) return rc;
    int fd = waywallen_display_get_fd(d);
    for (;;) {
        rc = waywallen_display_advance_handshake(d);
        if (rc == WAYWALLEN_HS_DONE) return WAYWALLEN_OK;
        if (rc < 0) return rc;
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.revents = 0;
        if (rc == WAYWALLEN_HS_NEED_READ)       pfd.events = POLLIN;
        else if (rc == WAYWALLEN_HS_NEED_WRITE) pfd.events = POLLOUT;
        else                                    pfd.events = POLLIN | POLLOUT;
        int n = poll(&pfd, 1, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            return WAYWALLEN_ERR_IO;
        }
    }
}

int waywallen_display_connect(waywallen_display_t *d,
                              const char *socket_path,
                              const char *display_name,
                              uint32_t width,
                              uint32_t height,
                              uint32_t refresh_mhz) {
    return waywallen_display_connect_v2(d, socket_path, display_name, NULL,
                                        width, height, refresh_mhz);
}

int waywallen_display_update_size(waywallen_display_t *d,
                                  uint32_t width,
                                  uint32_t height) {
    if (!d) return WAYWALLEN_ERR_INVAL;
    if (d->conn != WW_CONN_CONNECTED) {
        return WAYWALLEN_ERR_STATE;
    }
    ww_req_update_display_t upd;
    memset(&upd, 0, sizeof(upd));
    upd.width = width;
    upd.height = height;
    upd.properties.count = 0;
    upd.properties.data = NULL;
    int rc = send_request_msg(d, WW_REQ_UPDATE_DISPLAY, enc_update, &upd);
    if (rc != WAYWALLEN_OK) {
        fire_disconnected(d, rc, "send update_display");
    }
    return rc;
}

int waywallen_display_get_fd(waywallen_display_t *d) {
    if (!d) return -1;
    return d->fd;
}

/* ------------------------------------------------------------------ */
/*  Dispatch                                                           */
/* ------------------------------------------------------------------ */

#ifdef WW_HAVE_EGL
/* Destroy any cached EGL resources associated with the current bound
 * pool. Called from fire_textures_releasing_if_any and on disconnect. */
static void egl_release_current_pool(waywallen_display_t *d) {
    if (!d->egl_backend.loaded || d->egl_import_count == 0) {
        free(d->egl_images);
        free(d->egl_gl_textures);
        d->egl_images = NULL;
        d->egl_gl_textures = NULL;
        d->egl_import_count = 0;
        return;
    }
    if (d->egl_gl_textures) {
        d->egl_backend.glDeleteTextures((int)d->egl_import_count,
                                        d->egl_gl_textures);
    }
    if (d->egl_images) {
        for (uint32_t i = 0; i < d->egl_import_count; i++) {
            if (d->egl_images[i]) {
                ww_egl_destroy_image(&d->egl_backend, d->egl.egl_display,
                                     (EGLImageKHR)d->egl_images[i]);
            }
        }
    }
    free(d->egl_images);
    free(d->egl_gl_textures);
    d->egl_images = NULL;
    d->egl_gl_textures = NULL;
    d->egl_import_count = 0;
}
#endif

#ifdef WW_HAVE_VULKAN
static void vk_release_current_pool(waywallen_display_t *d) {
    if (!d->vk_backend.loaded || d->vk_import_count == 0) {
        free(d->vk_images);
        free(d->vk_semaphores);
        d->vk_images = NULL;
        d->vk_semaphores = NULL;
        d->vk_import_count = 0;
        return;
    }
    /* Drain the device before tearing down imported VkImages /
     * VkDeviceMemory. Consumers (e.g. the QML plugin) clear their
     * pointer caches in on_textures_releasing but do not synchronize
     * with their own render thread, so without this barrier in-flight
     * descriptor writes / command buffers reference freed memory
     * (UNASSIGNED-VkDescriptorImageInfo-BoundResourceFreedMemoryAccess
     * and VUID-VkImageMemoryBarrier-image-parameter). */
    if (d->vk_backend.vkDeviceWaitIdle) {
        VkResult wr = d->vk_backend.vkDeviceWaitIdle(d->vk_backend.device);
        if (wr != VK_SUCCESS) {
            ww_log(WAYWALLEN_LOG_WARN,
                   "vk_release_current_pool: vkDeviceWaitIdle returned %d; "
                   "destroying anyway",
                   (int)wr);
        }
    }
    if (d->vk_images) {
        for (uint32_t i = 0; i < d->vk_import_count; i++) {
            ww_vk_destroy_imported_image(&d->vk_backend, &d->vk_images[i]);
        }
    }
    if (d->vk_semaphores) {
        for (uint32_t i = 0; i < d->vk_import_count; i++) {
            if (d->vk_semaphores[i] != VK_NULL_HANDLE) {
                d->vk_backend.vkDestroySemaphore(
                    d->vk_backend.device, d->vk_semaphores[i], NULL);
            }
        }
    }
    free(d->vk_images);
    free(d->vk_semaphores);
    d->vk_images = NULL;
    d->vk_semaphores = NULL;
    d->vk_import_count = 0;
}
#endif

static void fire_textures_releasing_if_any(waywallen_display_t *d) {
    if (!d->has_textures) return;
    if (d->cb.on_textures_releasing) {
        d->cb.on_textures_releasing(d->cb.user_data, &d->current_textures);
    }
#ifdef WW_HAVE_EGL
    egl_release_current_pool(d);
#endif
#ifdef WW_HAVE_VULKAN
    vk_release_current_pool(d);
#endif
    /* Free the void** handle arrays we built for the callback payload. */
    free(d->current_textures.vk_images);
    free(d->current_textures.vk_memories);
    memset(&d->current_textures, 0, sizeof(d->current_textures));
    d->has_textures = false;
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
static int try_egl_import(waywallen_display_t *d,
                          const ww_evt_bind_buffers_t *bb,
                          int *fd_buf, size_t n_fds) {
    if (!d->egl_backend.loaded) return -ENOSYS;
    if (bb->planes_per_buffer == 0
        || bb->planes_per_buffer > WW_EGL_MAX_PLANES) {
        return -EINVAL;
    }
    if (bb->count == 0) return -EINVAL;
    if (bb->stride.count != n_fds
        || bb->plane_offset.count != n_fds) {
        return -EINVAL;
    }

    d->egl_import_count = 0;
    d->egl_images = (void **)calloc(bb->count, sizeof(void *));
    d->egl_gl_textures = (uint32_t *)calloc(bb->count, sizeof(uint32_t));
    if (!d->egl_images || !d->egl_gl_textures) {
        free(d->egl_images);
        free(d->egl_gl_textures);
        d->egl_images = NULL;
        d->egl_gl_textures = NULL;
        return -ENOMEM;
    }

    for (uint32_t b = 0; b < bb->count; b++) {
        ww_egl_dmabuf_import_t im;
        memset(&im, 0, sizeof(im));
        im.egl_display = d->egl.egl_display;
        im.fourcc = bb->fourcc;
        im.width = bb->width;
        im.height = bb->height;
        im.modifier = bb->modifier;
        im.n_planes = bb->planes_per_buffer;
        for (uint32_t p = 0; p < bb->planes_per_buffer; p++) {
            size_t idx = (size_t)b * bb->planes_per_buffer + p;
            im.fds[p] = fd_buf[idx];
            im.strides[p] = bb->stride.data[idx];
            im.offsets[p] = bb->plane_offset.data[idx];
        }
        EGLImageKHR img;
        int rc = ww_egl_import_dmabuf(&d->egl_backend, &im, &img);
        if (rc != 0) {
            for (uint32_t j = 0; j < b; j++) {
                if (d->egl_images[j]) {
                    ww_egl_destroy_image(&d->egl_backend, d->egl.egl_display,
                                         (EGLImageKHR)d->egl_images[j]);
                }
            }
            free(d->egl_images);
            free(d->egl_gl_textures);
            d->egl_images = NULL;
            d->egl_gl_textures = NULL;
            return rc;
        }
        d->egl_images[b] = (void *)img;
        /* GL texture creation is deferred to the host's render thread
         * via waywallen_display_create_gl_texture(). */
    }

    d->egl_import_count = bb->count;
    close_all_fds(fd_buf, n_fds);
    return 0;
}
#endif  /* WW_HAVE_EGL */

#ifdef WW_HAVE_VULKAN
static int try_vk_import(waywallen_display_t *d,
                         const ww_evt_bind_buffers_t *bb,
                         int *fd_buf, size_t n_fds) {
    if (!d->vk_backend.loaded) return -ENOSYS;
    if (bb->planes_per_buffer == 0
        || bb->planes_per_buffer > WW_VK_MAX_PLANES) return -EINVAL;
    if (bb->count == 0) return -EINVAL;
    if (bb->stride.count != n_fds || bb->plane_offset.count != n_fds)
        return -EINVAL;

    d->vk_import_count = 0;
    d->vk_images = (ww_vk_imported_image_t *)calloc(
        bb->count, sizeof(ww_vk_imported_image_t));
    d->vk_semaphores = (VkSemaphore *)calloc(bb->count, sizeof(VkSemaphore));
    if (!d->vk_images || !d->vk_semaphores) {
        free(d->vk_images);
        free(d->vk_semaphores);
        d->vk_images = NULL;
        d->vk_semaphores = NULL;
        return -ENOMEM;
    }

    /* Create one semaphore per slot for sync_fd import. */
    for (uint32_t b = 0; b < bb->count; b++) {
        VkSemaphoreCreateInfo sci = {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        };
        VkResult vr = d->vk_backend.vkCreateSemaphore(
            d->vk_backend.device, &sci, NULL, &d->vk_semaphores[b]);
        if (vr != VK_SUCCESS) goto rollback;
    }

    for (uint32_t b = 0; b < bb->count; b++) {
        ww_vk_dmabuf_import_t im;
        memset(&im, 0, sizeof(im));
        im.fourcc = bb->fourcc;
        im.width = bb->width;
        im.height = bb->height;
        im.modifier = bb->modifier;
        im.n_planes = bb->planes_per_buffer;
        for (uint32_t p = 0; p < bb->planes_per_buffer; p++) {
            size_t idx = (size_t)b * bb->planes_per_buffer + p;
            im.fds[p] = fd_buf[idx];
            im.strides[p] = bb->stride.data[idx];
            im.offsets[p] = bb->plane_offset.data[idx];
        }
        int rc = ww_vk_import_dmabuf(&d->vk_backend, &im, &d->vk_images[b]);
        if (rc != 0) goto rollback;
    }

    d->vk_import_count = bb->count;
    /* vkAllocateMemory took ownership of fds[0] per buffer on success.
     * Close remaining plane fds for multi-plane (planes > 0). For
     * single-plane (common case), all fds were consumed. */
    for (size_t i = 0; i < n_fds; i++) {
        /* For single-plane: fd was consumed by vkAllocateMemory; the
         * kernel already closed our copy. For safety, mark invalid. */
        fd_buf[i] = -1;
    }
    return 0;

rollback:
    for (uint32_t j = 0; j < bb->count; j++) {
        ww_vk_destroy_imported_image(&d->vk_backend, &d->vk_images[j]);
        if (d->vk_semaphores[j] != VK_NULL_HANDLE) {
            d->vk_backend.vkDestroySemaphore(
                d->vk_backend.device, d->vk_semaphores[j], NULL);
        }
    }
    free(d->vk_images);
    free(d->vk_semaphores);
    d->vk_images = NULL;
    d->vk_semaphores = NULL;
    return -EIO;
}
#endif  /* WW_HAVE_VULKAN */

static int handle_bind_buffers(waywallen_display_t *d,
                               const uint8_t *body, size_t body_len,
                               int *fd_buf, size_t n_fds) {
    /* Close any prior texture state first. */
    fire_textures_releasing_if_any(d);

    ww_evt_bind_buffers_t bb;
    if (ww_evt_bind_buffers_decode(body, body_len, &bb) != WW_OK) {
        close_all_fds(fd_buf, n_fds);
        fire_disconnected(d, WAYWALLEN_ERR_PROTO, "decode bind_buffers");
        return WAYWALLEN_ERR_PROTO;
    }
    /* NB: the daemon→display bind_buffers protocol does NOT carry the
     * producer's `flags` field (HOST_VISIBLE etc.) — that's only on the
     * producer↔daemon bridge protocol. So the placement-mode story
     * has to be inferred from the producer's logs + the import outcome. */
    ww_log(WAYWALLEN_LOG_INFO,
           "bind_buffers received gen=%" PRIu64 " count=%u %ux%u "
           "fourcc=0x%08x modifier=0x%" PRIx64 " planes_per_buffer=%u",
           bb.buffer_generation, bb.count, bb.width, bb.height,
           bb.fourcc, bb.modifier, bb.planes_per_buffer);
    uint32_t expected = bb.count * bb.planes_per_buffer;
    if ((size_t)expected != n_fds) {
        close_all_fds(fd_buf, n_fds);
        ww_evt_bind_buffers_free(&bb);
        fire_disconnected(d, WAYWALLEN_ERR_PROTO,
                          "bind_buffers fd count mismatch");
        return WAYWALLEN_ERR_PROTO;
    }

    waywallen_backend_t reported_backend = WAYWALLEN_BACKEND_NONE;
    void **reported_egl_images = NULL;
    uint32_t *reported_gl_textures = NULL;
    void **reported_vk_images = NULL;
    void **reported_vk_memories = NULL;
    int fds_consumed = 0;

#ifdef WW_HAVE_EGL
    if (!fds_consumed && d->backend == WAYWALLEN_BACKEND_EGL
        && d->egl_backend.loaded) {
        int ir = try_egl_import(d, &bb, fd_buf, n_fds);
        if (ir == 0) {
            ww_log(WAYWALLEN_LOG_INFO, "EGL import: %u images, %ux%u fourcc=0x%x",
                   bb.count, bb.width, bb.height, bb.fourcc);
            reported_backend = WAYWALLEN_BACKEND_EGL;
            reported_egl_images = d->egl_images;
            fds_consumed = 1;
        } else {
            ww_log(WAYWALLEN_LOG_WARN, "EGL import failed: %d", ir);
        }
    }
#endif
#ifdef WW_HAVE_VULKAN
    if (!fds_consumed && d->backend == WAYWALLEN_BACKEND_VULKAN
        && d->vk_backend.loaded) {
        int ir = try_vk_import(d, &bb, fd_buf, n_fds);
        if (ir == 0) {
            ww_log(WAYWALLEN_LOG_INFO, "Vulkan import: %u images, %ux%u fourcc=0x%x",
                   bb.count, bb.width, bb.height, bb.fourcc);
            reported_backend = WAYWALLEN_BACKEND_VULKAN;
            /* Build void* arrays pointing into the imported image
             * structs so the callback payload matches the public API. */
            reported_vk_images = (void **)calloc(bb.count, sizeof(void *));
            reported_vk_memories = (void **)calloc(bb.count, sizeof(void *));
            if (reported_vk_images && reported_vk_memories) {
                for (uint32_t i = 0; i < bb.count; i++) {
                    reported_vk_images[i] = (void *)d->vk_images[i].image;
                    reported_vk_memories[i] = (void *)d->vk_images[i].memory;
                }
            }
            fds_consumed = 1;
        } else {
            ww_log(WAYWALLEN_LOG_WARN, "Vulkan import failed: %d", ir);
        }
    }
#endif
    if (!fds_consumed) {
        ww_log(WAYWALLEN_LOG_WARN, "no backend imported buffers");
        close_all_fds(fd_buf, n_fds);
    }

    d->current_buffer_generation = bb.buffer_generation;
    d->current_textures.count = bb.count;
    d->current_textures.tex_width = bb.width;
    d->current_textures.tex_height = bb.height;
    d->current_textures.fourcc = bb.fourcc;
    d->current_textures.modifier = bb.modifier;
    d->current_textures.planes_per_buffer = bb.planes_per_buffer;
    d->current_textures.backend = reported_backend;
    d->current_textures.egl_images = reported_egl_images;
    d->current_textures.gl_textures = reported_gl_textures;
    d->current_textures.vk_images = reported_vk_images;
    d->current_textures.vk_memories = reported_vk_memories;
    d->has_textures = true;

    ww_evt_bind_buffers_free(&bb);
    d->stream = WW_STREAM_ACTIVE;

    if (d->cb.on_textures_ready) {
        d->cb.on_textures_ready(d->cb.user_data, &d->current_textures);
    }
    return WAYWALLEN_OK;
}

static int handle_set_config(waywallen_display_t *d,
                             const uint8_t *body, size_t body_len) {
    ww_evt_set_config_t sc;
    if (ww_evt_set_config_decode(body, body_len, &sc) != WW_OK) {
        fire_disconnected(d, WAYWALLEN_ERR_PROTO, "decode set_config");
        return WAYWALLEN_ERR_PROTO;
    }
    /* Only valid after bind_buffers. */
    if (d->stream != WW_STREAM_ACTIVE) {
        ww_evt_set_config_free(&sc);
        fire_disconnected(d, WAYWALLEN_ERR_PROTO,
                          "set_config in invalid state");
        return WAYWALLEN_ERR_PROTO;
    }
    waywallen_config_t cfg;
    cfg.source_rect.x = sc.source_rect.x;
    cfg.source_rect.y = sc.source_rect.y;
    cfg.source_rect.w = sc.source_rect.w;
    cfg.source_rect.h = sc.source_rect.h;
    cfg.dest_rect.x = sc.dest_rect.x;
    cfg.dest_rect.y = sc.dest_rect.y;
    cfg.dest_rect.w = sc.dest_rect.w;
    cfg.dest_rect.h = sc.dest_rect.h;
    cfg.transform = sc.transform;
    cfg.clear_color[0] = sc.clear_r;
    cfg.clear_color[1] = sc.clear_g;
    cfg.clear_color[2] = sc.clear_b;
    cfg.clear_color[3] = sc.clear_a;
    ww_evt_set_config_free(&sc);

    /* stream stays ACTIVE */

    if (d->cb.on_config) {
        d->cb.on_config(d->cb.user_data, &cfg);
    }
    return WAYWALLEN_OK;
}

static int handle_frame_ready(waywallen_display_t *d,
                              const uint8_t *body, size_t body_len,
                              int *fd_buf, size_t n_fds) {
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
        fire_disconnected(d, WAYWALLEN_ERR_PROTO,
                          "frame_ready expected 2 fds");
        return WAYWALLEN_ERR_PROTO;
    }
    int acquire_fd = fd_buf[0];
    int release_syncobj_fd = fd_buf[1];
    if (d->stream != WW_STREAM_ACTIVE) {
        close(acquire_fd);
        close(release_syncobj_fd);
        ww_evt_frame_ready_free(&fr);
        fire_disconnected(d, WAYWALLEN_ERR_PROTO,
                          "frame_ready in wrong state");
        return WAYWALLEN_ERR_PROTO;
    }
    /* Drop stale-generation frames silently. */
    if (fr.buffer_generation != d->current_buffer_generation) {
        close(acquire_fd);
        close(release_syncobj_fd);
        ww_evt_frame_ready_free(&fr);
        return WAYWALLEN_OK;
    }

    int fd_handled = 0;
    void *acquire_semaphore = NULL;

#ifdef WW_HAVE_EGL
    if (!fd_handled && d->backend == WAYWALLEN_BACKEND_EGL
        && d->egl_backend.loaded) {
        int rc = ww_egl_wait_sync_fd(&d->egl_backend, d->egl.egl_display,
                                     acquire_fd);
        if (rc != 0) {
            close(acquire_fd);
        }
        fd_handled = 1;
    }
#endif
#ifdef WW_HAVE_VULKAN
    if (!fd_handled && d->backend == WAYWALLEN_BACKEND_VULKAN
        && d->vk_backend.loaded && d->vk_semaphores) {
        uint32_t slot = fr.buffer_index;
        if (slot < d->vk_import_count && d->vk_semaphores[slot] != VK_NULL_HANDLE) {
            int rc = ww_vk_import_sync_fd(&d->vk_backend,
                                          d->vk_semaphores[slot],
                                          acquire_fd);
            if (rc == 0) {
                acquire_semaphore = (void *)d->vk_semaphores[slot];
            } else {
                close(acquire_fd);
            }
        } else {
            close(acquire_fd);
        }
        fd_handled = 1;
    }
#endif
    if (!fd_handled) {
        close(acquire_fd);
    }

    waywallen_frame_t frame;
    frame.buffer_index = fr.buffer_index;
    frame.seq = fr.seq;
    frame.vk_acquire_semaphore = acquire_semaphore;
    /* Hand the raw release_syncobj fd to the host. Ownership transfers:
     * the host MUST signal it from its release GPU work and then close. */
    frame.release_syncobj_fd = release_syncobj_fd;
    ww_evt_frame_ready_free(&fr);

    if (d->cb.on_frame_ready) {
        d->cb.on_frame_ready(d->cb.user_data, &frame);
    } else {
        /* No callback to consume the release fd: close it so the daemon
         * eventually times out the frame instead of leaking the fd. */
        close(release_syncobj_fd);
    }
    return WAYWALLEN_OK;
}

static int handle_unbind(waywallen_display_t *d,
                         const uint8_t *body, size_t body_len) {
    ww_evt_unbind_t ub;
    if (ww_evt_unbind_decode(body, body_len, &ub) != WW_OK) {
        fire_disconnected(d, WAYWALLEN_ERR_PROTO, "decode unbind");
        return WAYWALLEN_ERR_PROTO;
    }
    ww_evt_unbind_free(&ub);
    fire_textures_releasing_if_any(d);
    d->stream = WW_STREAM_INACTIVE;
    return WAYWALLEN_OK;
}

static int handle_error(waywallen_display_t *d,
                        const uint8_t *body, size_t body_len) {
    ww_evt_error_t er;
    if (ww_evt_error_decode(body, body_len, &er) != WW_OK) {
        fire_disconnected(d, WAYWALLEN_ERR_PROTO, "decode error");
        return WAYWALLEN_ERR_PROTO;
    }
    /* Make a local copy of the message before freeing `er`, then
     * fire the callback. */
    char *msg = er.message;
    er.message = NULL;
    ww_evt_error_free(&er);
    fire_disconnected(d, WAYWALLEN_ERR_PROTO, msg ? msg : "server error");
    free(msg);
    return WAYWALLEN_ERR_PROTO;
}

int waywallen_display_dispatch(waywallen_display_t *d) {
    if (!d) return WAYWALLEN_ERR_INVAL;
    if (d->fd < 0 || d->conn == WW_CONN_DEAD) return WAYWALLEN_ERR_NOTCONN;
    if (d->conn == WW_CONN_DISCONNECTED) return WAYWALLEN_ERR_STATE;

    static uint8_t body_buf[WW_CODEC_MAX_BODY_BYTES];
    uint16_t op;
    size_t body_len;
    int fd_buf[WW_CODEC_MAX_FDS_PER_MSG];
    size_t n_fds;
    int rc = recv_event_frame(d, &op, body_buf, &body_len, fd_buf,
                              WW_CODEC_MAX_FDS_PER_MSG, &n_fds);
    if (rc != WAYWALLEN_OK) {
        fire_disconnected(d, rc, "recv event");
        return rc;
    }

    switch (op) {
        case WW_EVT_BIND_BUFFERS:
            return handle_bind_buffers(d, body_buf, body_len, fd_buf, n_fds);
        case WW_EVT_SET_CONFIG:
            close_all_fds(fd_buf, n_fds);
            return handle_set_config(d, body_buf, body_len);
        case WW_EVT_FRAME_READY:
            return handle_frame_ready(d, body_buf, body_len, fd_buf, n_fds);
        case WW_EVT_UNBIND:
            close_all_fds(fd_buf, n_fds);
            return handle_unbind(d, body_buf, body_len);
        case WW_EVT_ERROR:
            close_all_fds(fd_buf, n_fds);
            return handle_error(d, body_buf, body_len);
        case WW_EVT_WELCOME:
        case WW_EVT_DISPLAY_ACCEPTED:
            /* Legal only during handshake. Seeing them again is a
             * protocol violation. */
            close_all_fds(fd_buf, n_fds);
            fire_disconnected(d, WAYWALLEN_ERR_PROTO,
                              "unexpected handshake event");
            return WAYWALLEN_ERR_PROTO;
        default:
            /* Unknown opcodes are forward-compat: log + drop. */
            close_all_fds(fd_buf, n_fds);
            return WAYWALLEN_OK;
    }
}

/* ------------------------------------------------------------------ */
/*  Release / disconnect                                               */
/* ------------------------------------------------------------------ */

int waywallen_display_release_frame(waywallen_display_t *d,
                                    uint32_t buffer_index,
                                    uint64_t seq) {
    /* DEPRECATED — v1 dropped the BufferRelease wire request. Release
     * is now signaled via the per-frame `release_syncobj_fd` on the
     * frame_ready callback. This stub remains so existing callers
     * link, but it does not communicate with the daemon. */
    (void)buffer_index;
    (void)seq;
    if (!d) return WAYWALLEN_ERR_INVAL;
    return WAYWALLEN_OK;
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
struct ww_drm_syncobj_destroy {
    uint32_t handle;
    uint32_t pad;
};
struct ww_drm_syncobj_array {
    uint64_t handles;
    uint32_t count_handles;
    uint32_t pad;
};

#ifndef DRM_IOCTL_BASE
#define DRM_IOCTL_BASE 'd'
#endif
#define WW_DRM_IOCTL_SYNCOBJ_DESTROY \
    _IOWR(DRM_IOCTL_BASE, 0xC0, struct ww_drm_syncobj_destroy)
#define WW_DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE \
    _IOWR(DRM_IOCTL_BASE, 0xC2, struct ww_drm_syncobj_handle)
#define WW_DRM_IOCTL_SYNCOBJ_SIGNAL \
    _IOWR(DRM_IOCTL_BASE, 0xC5, struct ww_drm_syncobj_array)

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
    uint32_t handles[1] = { imp.handle };
    struct ww_drm_syncobj_array sig = {
        .handles = (uintptr_t)handles,
        .count_handles = 1,
        .pad = 0,
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

void waywallen_display_disconnect(waywallen_display_t *d) {
    if (!d) return;
    if (d->fd >= 0) {
        /* Best-effort bye; ignore errors. Only meaningful once the
         * connection is fully established — sending bye mid-handshake
         * would just confuse the server. */
        if (d->conn == WW_CONN_CONNECTED) {
            ww_req_bye_t bye;
            memset(&bye, 0, sizeof(bye));
            (void)send_request_msg(d, WW_REQ_BYE, enc_bye, &bye);
        }
        close(d->fd);
        d->fd = -1;
    }
    fire_textures_releasing_if_any(d);
    d->conn = WW_CONN_DISCONNECTED;
    d->stream = WW_STREAM_INACTIVE;
    d->hs_state = WW_HS_IDLE;
    ww_codec_recv_state_reset(&d->hs_recv);
    d->hs_send_len = 0;
    d->hs_send_pos = 0;
}

/* ------------------------------------------------------------------ */
/*  State queries                                                      */
/* ------------------------------------------------------------------ */

waywallen_conn_state_t waywallen_display_conn_state(waywallen_display_t *d) {
    if (!d) return WAYWALLEN_CONN_DISCONNECTED;
    return (waywallen_conn_state_t)d->conn;
}

waywallen_stream_state_t waywallen_display_stream_state(waywallen_display_t *d) {
    if (!d) return WAYWALLEN_STREAM_INACTIVE;
    return (waywallen_stream_state_t)d->stream;
}

/* ------------------------------------------------------------------ */
/*  EGL deferred GL texture creation                                   */
/* ------------------------------------------------------------------ */

int waywallen_display_create_gl_texture(waywallen_display_t *d,
                                        uint32_t idx,
                                        uint32_t *out_gl_texture) {
#ifdef WW_HAVE_EGL
    if (!d || !out_gl_texture) return WAYWALLEN_ERR_INVAL;
    if (d->backend != WAYWALLEN_BACKEND_EGL || !d->egl_backend.loaded)
        return WAYWALLEN_ERR_STATE;
    if (idx >= d->egl_import_count || !d->egl_images)
        return WAYWALLEN_ERR_INVAL;
    if (!d->egl_images[idx])
        return WAYWALLEN_ERR_INVAL;

    /* Already created? Return the cached texture. */
    if (d->egl_gl_textures && d->egl_gl_textures[idx]) {
        *out_gl_texture = d->egl_gl_textures[idx];
        return WAYWALLEN_OK;
    }

    GLuint tex = 0;
    int rc = ww_egl_texture_from_image(&d->egl_backend,
                                        (EGLImageKHR)d->egl_images[idx],
                                        &tex);
    if (rc != 0) {
        ww_log(WAYWALLEN_LOG_WARN, "GL texture creation failed for image %u: %d", idx, rc);
        return WAYWALLEN_ERR_IO;
    }

    ww_log(WAYWALLEN_LOG_DEBUG, "created GL texture %u for image %u", tex, idx);
    d->egl_gl_textures[idx] = tex;
    *out_gl_texture = tex;
    return WAYWALLEN_OK;
#else
    (void)d; (void)idx; (void)out_gl_texture;
    return WAYWALLEN_ERR_NOT_IMPL;
#endif
}

void waywallen_display_delete_gl_texture(waywallen_display_t *d,
                                         uint32_t idx) {
#ifdef WW_HAVE_EGL
    if (!d || d->backend != WAYWALLEN_BACKEND_EGL) return;
    if (!d->egl_backend.loaded) return;
    if (idx >= d->egl_import_count) return;
    if (!d->egl_gl_textures || !d->egl_gl_textures[idx]) return;

    d->egl_backend.glDeleteTextures(1, &d->egl_gl_textures[idx]);
    d->egl_gl_textures[idx] = 0;
#else
    (void)d; (void)idx;
#endif
}
