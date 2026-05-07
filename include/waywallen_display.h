/*
 * libwaywallen_display — public C API for the waywallen-display-v1
 * wire protocol.
 *
 * This header is ABI-frozen for v0.1.x of the protocol. Backwards-
 * incompatible changes bump WAYWALLEN_DISPLAY_VERSION_MAJOR.
 *
 * Ownership rules:
 *   - All `waywallen_*` structs passed to callbacks are owned by the
 *     library. Host must not free them or the pointers they contain.
 *   - `char *` members of event callbacks are valid only for the
 *     duration of the callback invocation; copy before returning.
 *   - File descriptors the library receives from the wire (dma_buf
 *     fds, acquire sync_fds) are owned by the library and closed
 *     automatically. The host never sees raw fds in Phase 2.
 *
 * Threading:
 *   A single `waywallen_display_t` is NOT thread-safe. All methods
 *   except `waywallen_display_get_fd` must be called on the same
 *   thread that owns the host's render context. `get_fd` is safe to
 *   poll from an I/O thread as long as `dispatch` never runs
 *   concurrently with any other method on the same handle.
 *
 * Error handling:
 *   Functions return 0 on success and a negative errno-ish value on
 *   failure (see `ww_err_t` enum). On fatal session errors the
 *   library invokes `on_disconnected` and transitions to DEAD state;
 *   the host must call `disconnect` to clean up and may re-create or
 *   reconnect the handle.
 */

#ifndef WAYWALLEN_DISPLAY_H
#define WAYWALLEN_DISPLAY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WAYWALLEN_DISPLAY_VERSION_MAJOR 0
#define WAYWALLEN_DISPLAY_VERSION_MINOR 1

/*
 * On-the-wire protocol version. Independent of the library API/ABI
 * version above — this number tracks the `<protocol version="...">`
 * attribute in waywallen_display_v1.xml and is sent verbatim in
 * `hello.client_protocol_version`. The daemon owns the supported
 * range and rejects out-of-range clients with `error{code=2}`.
 */
#define WAYWALLEN_DISPLAY_PROTOCOL_VERSION 5

/* -------------------------------------------------------------------------
 * Return codes
 * ------------------------------------------------------------------------- */

typedef enum waywallen_err {
    WAYWALLEN_OK = 0,
    WAYWALLEN_ERR_INVAL = -1,       /* bad argument */
    WAYWALLEN_ERR_NOMEM = -2,       /* allocation failed */
    WAYWALLEN_ERR_STATE = -3,       /* called in wrong state */
    WAYWALLEN_ERR_IO = -4,          /* socket / syscall error */
    WAYWALLEN_ERR_PROTO = -5,       /* wire format / state machine violation */
    WAYWALLEN_ERR_NOTCONN = -6,     /* not connected */
    WAYWALLEN_ERR_NOT_IMPL = -7,    /* feature not implemented in this phase */
} waywallen_err_t;

/* -------------------------------------------------------------------------
 * Forward type
 * ------------------------------------------------------------------------- */

typedef struct waywallen_display waywallen_display_t;

/* -------------------------------------------------------------------------
 * Logging
 *
 * By default the library logs to stderr. Call `set_log_callback` to
 * redirect messages to the host's logging framework (e.g. Qt's
 * QLoggingCategory, syslog, etc.).
 * ------------------------------------------------------------------------- */

typedef enum waywallen_log_level {
    WAYWALLEN_LOG_DEBUG = 0,
    WAYWALLEN_LOG_INFO  = 1,
    WAYWALLEN_LOG_WARN  = 2,
    WAYWALLEN_LOG_ERROR = 3,
} waywallen_log_level_t;

typedef void (*waywallen_log_callback_t)(waywallen_log_level_t level,
                                         const char *msg,
                                         void *user_data);

void waywallen_display_set_log_callback(waywallen_log_callback_t cb,
                                        void *user_data);

/* -------------------------------------------------------------------------
 * Connection + stream state
 *
 * The display tracks two independent state dimensions:
 *   conn_state  — socket connection to the backend daemon
 *   stream_state — whether the daemon is actively pushing DMA-BUF frames
 *
 * The display does NOT know or care about renderer processes. It only
 * sees: "am I connected to the backend?" and "is the backend sending
 * me frames?"
 * ------------------------------------------------------------------------- */

typedef enum waywallen_conn_state {
    WAYWALLEN_CONN_DISCONNECTED = 0,
    WAYWALLEN_CONN_CONNECTING,
    WAYWALLEN_CONN_CONNECTED,
    WAYWALLEN_CONN_DEAD,
} waywallen_conn_state_t;

typedef enum waywallen_stream_state {
    WAYWALLEN_STREAM_INACTIVE = 0,
    WAYWALLEN_STREAM_ACTIVE,
} waywallen_stream_state_t;

/*
 * Handshake state for the async connect path. Drives the explicit
 * state machine exposed via waywallen_display_advance_handshake().
 * Hosts that just want a one-shot connect should use the legacy
 * waywallen_display_connect() (which wraps this internally).
 */
typedef enum waywallen_handshake_state {
    WAYWALLEN_HS_IDLE          = 0, /* before begin_connect / after disconnect */
    WAYWALLEN_HS_CONNECTING    = 1, /* connect(2) returned EINPROGRESS         */
    WAYWALLEN_HS_HELLO_PENDING = 2, /* hello queued, partial sendmsg           */
    WAYWALLEN_HS_WELCOME_WAIT  = 3, /* hello fully sent, waiting POLLIN        */
    WAYWALLEN_HS_REGISTER_PEND = 4, /* welcome decoded, register queued        */
    WAYWALLEN_HS_ACCEPTED_WAIT = 5, /* register fully sent, waiting POLLIN     */
    WAYWALLEN_HS_READY         = 6, /* DISPLAY_ACCEPTED received, dispatch OK  */
} waywallen_handshake_state_t;

/* Return codes from waywallen_display_advance_handshake(). Negative
 * values are propagated waywallen_err_t / -errno. */
#define WAYWALLEN_HS_DONE       1   /* state transitioned to READY        */
#define WAYWALLEN_HS_NEED_READ  2   /* arm POLLIN, call again on readable */
#define WAYWALLEN_HS_NEED_WRITE 3   /* arm POLLOUT, call again on writable*/
#define WAYWALLEN_HS_PROGRESS   4   /* state advanced, more I/O needed    */

/* -------------------------------------------------------------------------
 * Backends (Phase 2: enum is recorded but not honoured beyond that)
 * ------------------------------------------------------------------------- */

typedef enum waywallen_backend {
    WAYWALLEN_BACKEND_NONE = 0,
    WAYWALLEN_BACKEND_EGL = 1,
    WAYWALLEN_BACKEND_VULKAN = 2,
} waywallen_backend_t;

typedef struct waywallen_egl_ctx {
    void *egl_display;                        /* EGLDisplay */
    void *(*get_proc_address)(const char *);  /* may be NULL */
} waywallen_egl_ctx_t;

typedef struct waywallen_vk_ctx {
    void *instance;                           /* VkInstance */
    void *physical_device;                    /* VkPhysicalDevice */
    void *device;                             /* VkDevice */
    uint32_t queue_family_index;
    /* PFN_vkGetInstanceProcAddr. May be NULL — the backend will then
     * dlopen("libvulkan.so.1") and resolve vkGetInstanceProcAddr from
     * there. The library never links libvulkan.so directly. */
    void *(*vk_get_instance_proc_addr)(void *instance, const char *name);
} waywallen_vk_ctx_t;

/* -------------------------------------------------------------------------
 * Callback payloads
 * ------------------------------------------------------------------------- */

typedef struct waywallen_rect {
    float x, y, w, h;
} waywallen_rect_t;

/* Texture set handed to the host after a successful bind_buffers.
 *
 * Phase 2: `backend == NONE`, all handle arrays are NULL, only the
 * descriptor metadata (count / width / height / fourcc / modifier) is
 * meaningful. Phase 3 fills in the real handles based on the backend
 * the host selected via `bind_egl` / `bind_vulkan`.
 */
typedef struct waywallen_textures {
    uint32_t count;
    uint32_t tex_width;
    uint32_t tex_height;
    uint32_t fourcc;
    uint64_t modifier;
    uint32_t planes_per_buffer;

    waywallen_backend_t backend;
    /* Only one set of handles is non-NULL, depending on `backend`. */
    void **egl_images;           /* length == count; each is an EGLImageKHR (EGL) */
    uint32_t *gl_textures;       /* length == count; created by host via
                                  * waywallen_display_create_gl_texture (EGL) */
    void **vk_images;            /* length == count; each is a VkImage (Vulkan) */
    void **vk_memories;          /* length == count; each is a VkDeviceMemory (Vulkan) */
} waywallen_textures_t;

typedef struct waywallen_config {
    waywallen_rect_t source_rect;   /* in texture pixels */
    waywallen_rect_t dest_rect;     /* in display pixels */
    uint32_t transform;             /* wl_output.transform bits */
    float clear_color[4];           /* RGBA straight alpha */
} waywallen_config_t;

typedef struct waywallen_frame {
    uint32_t buffer_index;
    uint64_t seq;
    /* Imported acquire semaphore (VkSemaphore). NULL if the library
     * could not import (e.g. EGL backend, or VK import failed); the
     * acquire sync_fd was closed in that case. The host must attach
     * this to its next `VkQueueSubmit`'s wait list. */
    void *vk_acquire_semaphore;
    /* Raw drm_syncobj fd (binary, unsignaled). Ownership transfers
     * to the host: the host MUST signal it from its release GPU work
     * (typically via `vkImportSemaphoreFdKHR(OPAQUE_FD)` on a binary
     * semaphore signaled by the consume submit, then `close(2)`).
     * If the host cannot signal it, `close(2)` it anyway — the daemon
     * will time out the corresponding frame. -1 means the library
     * could not extract a release_syncobj fd from this frame_ready. */
    int release_syncobj_fd;
} waywallen_frame_t;

/* -------------------------------------------------------------------------
 * Callback table
 *
 * Any callback pointer may be NULL; the library will silently skip
 * invoking a NULL callback. A non-NULL `on_disconnected` is strongly
 * recommended.
 * ------------------------------------------------------------------------- */

typedef struct waywallen_display_callbacks {
    void (*on_textures_ready)(void *user_data,
                              const waywallen_textures_t *textures);
    void (*on_textures_releasing)(void *user_data,
                                  const waywallen_textures_t *textures);
    void (*on_config)(void *user_data,
                      const waywallen_config_t *config);
    void (*on_frame_ready)(void *user_data,
                           const waywallen_frame_t *frame);
    void (*on_disconnected)(void *user_data,
                            int err_code,
                            const char *message);
    void *user_data;
} waywallen_display_callbacks_t;

/* -------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------- */

waywallen_display_t *waywallen_display_new(const waywallen_display_callbacks_t *cb);
void                 waywallen_display_destroy(waywallen_display_t *d);

/* -------------------------------------------------------------------------
 * Backend binding (must be called before `connect`)
 * ------------------------------------------------------------------------- */

int waywallen_display_bind_egl(waywallen_display_t *d,
                               const waywallen_egl_ctx_t *ctx);
int waywallen_display_bind_vulkan(waywallen_display_t *d,
                                  const waywallen_vk_ctx_t *ctx);

/*
 * Override the DRM render-node id reported during register_display.
 * Pass `(0, 0)` to mark "unknown" — the daemon will then conservatively
 * force HOST_VISIBLE buffer placement so the dmabuf can be PRIME-imported
 * across GPUs.
 *
 * Hosts normally don't need to call this: bind_egl / bind_vulkan
 * introspect the bound EGLDisplay / VkPhysicalDevice and populate the
 * id automatically (via `EGL_EXT_device_query` / `VK_EXT_physical_device_drm`).
 * Explicit overrides are useful for tests or for hosts that pick a render
 * node out of band.
 *
 * Must be called before `begin_connect` / `connect`.
 */
int waywallen_display_set_drm_render_node(waywallen_display_t *d,
                                          uint32_t major,
                                          uint32_t minor);

/* -------------------------------------------------------------------------
 * Async session (event-loop friendly)
 *
 * Modeled after libwayland's prepare_read / read_events split. Three
 * primitives drive the handshake from the host's main event loop:
 *
 *   1. `begin_connect` opens the UDS non-blocking, queues the `hello`
 *      request body in an internal buffer, and returns. The fd is then
 *      available via `get_fd` for the host to attach to its poll loop.
 *
 *   2. `advance_handshake` is invoked whenever the fd becomes readable
 *      and/or writable (per the previous return value). It runs the
 *      non-blocking state machine one step. Hosts arm POLLIN / POLLOUT
 *      based on the return value (NEED_READ / NEED_WRITE), then call
 *      again. PROGRESS means the machine moved forward and the caller
 *      should immediately call again (no I/O wait needed).
 *
 *   3. `handshake_state` is an introspection accessor.
 *
 * After a DONE return the connection is fully established and the host
 * may drop the write notifier and treat the fd like the legacy blocking
 * path: read-only QSocketNotifier driving `waywallen_display_dispatch`.
 * ------------------------------------------------------------------------- */

/*
 * `instance_id` (added in protocol v4): a stable identifier persisted by
 * the host (e.g. UUID4 stored in KDE/GNOME extension config) that
 * survives reconnects and DE restarts. Used by the daemon as the key
 * into per-display settings. Pass NULL or "" if the host has no stable
 * id; the daemon will fall back to indexing settings by display_name.
 */
int waywallen_display_begin_connect_v2(waywallen_display_t *d,
                                       const char *socket_path,
                                       const char *display_name,
                                       const char *instance_id,
                                       uint32_t width,
                                       uint32_t height,
                                       uint32_t refresh_mhz);

/* Legacy entry point — equivalent to `_v2` with instance_id=NULL. */
__attribute__((deprecated("use waywallen_display_begin_connect_v2")))
int waywallen_display_begin_connect(waywallen_display_t *d,
                                    const char *socket_path,
                                    const char *display_name,
                                    uint32_t width,
                                    uint32_t height,
                                    uint32_t refresh_mhz);

int waywallen_display_advance_handshake(waywallen_display_t *d);

waywallen_handshake_state_t waywallen_display_handshake_state(waywallen_display_t *d);

/* -------------------------------------------------------------------------
 * Session
 * ------------------------------------------------------------------------- */

/*
 * Convenience wrapper around begin_connect + a private poll loop +
 * advance_handshake. Blocks the calling thread until the handshake
 * completes (or fails). Suitable for one-shot CLI tools and tests;
 * NEVER call from a thread that runs an event loop — use the async
 * primitives above instead.
 *
 * See `_begin_connect_v2` for the meaning of `instance_id`.
 */
int waywallen_display_connect_v2(waywallen_display_t *d,
                                 const char *socket_path,
                                 const char *display_name,
                                 const char *instance_id,
                                 uint32_t width,
                                 uint32_t height,
                                 uint32_t refresh_mhz);

/* Legacy entry point — equivalent to `_v2` with instance_id=NULL. */
__attribute__((deprecated("use waywallen_display_connect_v2")))
int waywallen_display_connect(waywallen_display_t *d,
                              const char *socket_path,
                              const char *display_name,
                              uint32_t width,
                              uint32_t height,
                              uint32_t refresh_mhz);

int waywallen_display_update_size(waywallen_display_t *d,
                                  uint32_t width,
                                  uint32_t height);

/*
 * Read-side fd for poll(2) integration. Returns -1 if the display is
 * not currently connected.
 */
int waywallen_display_get_fd(waywallen_display_t *d);

/*
 * Consume whatever bytes are currently readable on the socket,
 * decode frames, fire callbacks synchronously. Safe to call from
 * a poll loop. Returns the number of frames dispatched on success
 * (may be 0), or a negative `waywallen_err_t` on failure. On fatal
 * failure `on_disconnected` has already been invoked.
 */
int waywallen_display_dispatch(waywallen_display_t *d);

/*
 * DEPRECATED — v1 dropped the BufferRelease wire request. Release is
 * now signaled by the host flipping the `release_syncobj_fd` that
 * arrived on the matching frame_ready. This function is a no-op kept
 * for ABI compatibility during the transition; new hosts should not
 * call it.
 */
int waywallen_display_release_frame(waywallen_display_t *d,
                                    uint32_t buffer_index,
                                    uint64_t seq);

/*
 * Convenience: SIGNAL the binary drm_syncobj at `fd` and close it.
 *
 * Use this from hosts that don't own a real GPU release fence
 * (e.g. blit-only consumers, headless probes) once they've finished
 * reading the dma-buf for the matching frame. The daemon's reaper is
 * waiting on this fd; signaling it lets the producer reuse the
 * buffer slot.
 *
 * Hosts with a Vulkan release fence should NOT call this — instead
 * import `release_syncobj_fd` as a binary VkSemaphore via
 * `vkImportSemaphoreFdKHR(VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT)`
 * and add it to the signal-semaphore list of the consume submit.
 *
 * `fd` ownership transfers in: closed on every return path.
 * Returns 0 on success or a negative `waywallen_err_t` on failure
 * (DRM render node missing, ioctl error). On error the fd has still
 * been closed.
 */
int waywallen_display_signal_release_syncobj(int fd);

void waywallen_display_disconnect(waywallen_display_t *d);

/* -------------------------------------------------------------------------
 * State queries
 * ------------------------------------------------------------------------- */

waywallen_conn_state_t   waywallen_display_conn_state(waywallen_display_t *d);
waywallen_stream_state_t waywallen_display_stream_state(waywallen_display_t *d);

/* -------------------------------------------------------------------------
 * EGL deferred GL texture creation
 *
 * When backend == EGL, `on_textures_ready` delivers EGLImageKHR handles
 * but no GL textures. The host calls these from a thread with a current
 * GL context (typically the render thread) to create / destroy the GL
 * texture objects.
 * ------------------------------------------------------------------------- */

int  waywallen_display_create_gl_texture(waywallen_display_t *d,
                                         uint32_t image_index,
                                         uint32_t *out_gl_texture);
void waywallen_display_delete_gl_texture(waywallen_display_t *d,
                                         uint32_t image_index);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* WAYWALLEN_DISPLAY_H */
