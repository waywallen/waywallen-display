/*
 * libwaywallen_display — public C API for the waywallen-display-v1
 * wire protocol.
 *
 * This header is ABI-frozen for v0.1.x of the protocol. Backwards-
 * incompatible changes bump WAYWALLEN_DISPLAY_VERSION_MAJOR.
 *
 * Phase status (read before using):
 *   Phase 2: lifecycle + state machine + codec are wired. Backend
 *            binding is a stub — `bind_egl` / `bind_vulkan` only
 *            record the choice; no texture import happens yet.
 *            `on_textures_ready` always reports `backend = NONE`.
 *   Phase 3+: real EGL and Vulkan DMA-BUF import.
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
    /* PFN_vkGetInstanceProcAddr; required, never NULL */
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
    /* Phase 2: always NULL. The library wait-imports nothing and
     * just closes the incoming acquire sync_fd. When the Vulkan
     * backend lands this will carry the imported `VkSemaphore` that
     * the host must attach to its next `VkQueueSubmit`'s wait list. */
    void *vk_acquire_semaphore;
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

/* -------------------------------------------------------------------------
 * Session
 * ------------------------------------------------------------------------- */

/*
 * Connect to `socket_path` (NULL = $XDG_RUNTIME_DIR/waywallen/display.sock),
 * perform the `hello` / `register_display` handshake, and return once the
 * server has accepted the registration. After success the caller is expected
 * to call `get_fd` and drive `dispatch` from its event loop.
 */
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
 * Send a buffer_release for the given `(buffer_index, seq)`. Phase 2
 * is synchronous: the caller must guarantee its GPU has finished
 * reading the buffer before calling this function.
 */
int waywallen_display_release_frame(waywallen_display_t *d,
                                    uint32_t buffer_index,
                                    uint64_t seq);

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
