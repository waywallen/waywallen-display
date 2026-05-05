/*
 * libwaywallen-gobject — GObject Introspection wrapper for
 * libwaywallen_display. Exposes namespace `Waywallen` to GJS / Vala.
 *
 * The wrapper deliberately mirrors the C ABI 1:1 with three
 * concessions to GIR ergonomics:
 *
 *   - Backends: only EGL is exposed in v0.1. Vulkan binding lives in
 *     the C ABI but isn't usefully callable from GJS without raw
 *     VkInstance/VkDevice handles.
 *
 *   - Frame ownership: `frame-ready` emits the release_syncobj fd as
 *     a plain int. The C trampoline holds the original; the JS handler
 *     calls `Waywallen.Display.signal_release_syncobj(fd)` (a class
 *     method that internally dups) to signal release. Trampoline closes
 *     the original after emit returns regardless.
 *
 *   - Boxed types: `WwTexturesInfo`, `WwFrameInfo`, `WwConfigInfo` are
 *     not exposed — signal payloads are flat primitives. Adding boxed
 *     types is straightforward later if richer payloads are needed.
 */

#ifndef WW_DISPLAY_GOBJECT_H
#define WW_DISPLAY_GOBJECT_H

#include <glib-object.h>

G_BEGIN_DECLS

/*
 * Mirror of waywallen_handshake_state_t. Kept in sync manually; if the
 * C ABI grows new states we update this enum and rerun g-ir-scanner.
 */
typedef enum {
    WW_HANDSHAKE_STATE_IDLE          = 0,
    WW_HANDSHAKE_STATE_CONNECTING    = 1,
    WW_HANDSHAKE_STATE_HELLO_PENDING = 2,
    WW_HANDSHAKE_STATE_WELCOME_WAIT  = 3,
    WW_HANDSHAKE_STATE_REGISTER_PEND = 4,
    WW_HANDSHAKE_STATE_ACCEPTED_WAIT = 5,
    WW_HANDSHAKE_STATE_READY         = 6,
} WwHandshakeState;

typedef enum {
    WW_HANDSHAKE_RESULT_DONE       = 1,
    WW_HANDSHAKE_RESULT_NEED_READ  = 2,
    WW_HANDSHAKE_RESULT_NEED_WRITE = 3,
    WW_HANDSHAKE_RESULT_PROGRESS   = 4,
} WwHandshakeResult;

typedef enum {
    WW_CONN_STATE_DISCONNECTED = 0,
    WW_CONN_STATE_CONNECTING   = 1,
    WW_CONN_STATE_CONNECTED    = 2,
    WW_CONN_STATE_DEAD         = 3,
} WwConnState;

typedef enum {
    WW_STREAM_STATE_INACTIVE = 0,
    WW_STREAM_STATE_ACTIVE   = 1,
} WwStreamState;

#define WW_TYPE_DISPLAY (ww_display_get_type())
G_DECLARE_FINAL_TYPE(WwDisplay, ww_display, WW, DISPLAY, GObject)

/**
 * ww_display_new:
 *
 * Allocate a fresh WwDisplay. Backend must be bound via
 * ww_display_bind_egl() before connecting.
 *
 * Returns: (transfer full): a new #WwDisplay
 */
WwDisplay *ww_display_new(void);

/**
 * ww_display_bind_egl:
 * @self: a #WwDisplay
 * @egl_display: (nullable): EGLDisplay handle as gpointer; pass NULL for
 *   EGL_DEFAULT_DISPLAY semantics handled by the underlying lib
 * @get_proc_address: (nullable): PFNEGLGETPROCADDRESS, gpointer-typed
 *
 * Must be called before ww_display_begin_connect().
 *
 * Returns: TRUE on success
 */
gboolean ww_display_bind_egl(WwDisplay *self,
                             gpointer egl_display,
                             gpointer get_proc_address);

/**
 * ww_display_set_drm_render_node:
 * @self: a #WwDisplay
 * @major: DRM render-node major
 * @minor: DRM render-node minor
 *
 * Returns: TRUE on success
 */
gboolean ww_display_set_drm_render_node(WwDisplay *self,
                                        guint major,
                                        guint minor);

/**
 * ww_display_begin_connect:
 * @self: a #WwDisplay
 * @socket_path: (nullable): UDS path; NULL for the daemon default
 * @display_name: human-readable name, also used for fallback settings key
 * @instance_id: (nullable): stable UUID v4 for per-display settings
 * @width: initial display width in pixels
 * @height: initial display height in pixels
 * @refresh_mhz: refresh rate in millihertz (e.g. 60000 for 60 Hz)
 *
 * Kicks off the async handshake. Caller drives it by polling
 * ww_display_get_fd() and looping ww_display_advance_handshake().
 *
 * Returns: TRUE on success
 */
gboolean ww_display_begin_connect(WwDisplay *self,
                                  const gchar *socket_path,
                                  const gchar *display_name,
                                  const gchar *instance_id,
                                  guint width,
                                  guint height,
                                  guint refresh_mhz);

/**
 * ww_display_advance_handshake:
 * @self: a #WwDisplay
 *
 * Returns: a #WwHandshakeResult code, or a negative errno-ish value on
 *   failure (which also fires `disconnected`).
 */
gint ww_display_advance_handshake(WwDisplay *self);

/**
 * ww_display_handshake_state:
 * @self: a #WwDisplay
 *
 * Returns: the current #WwHandshakeState
 */
WwHandshakeState ww_display_handshake_state(WwDisplay *self);

/**
 * ww_display_get_fd:
 * @self: a #WwDisplay
 *
 * Returns: the socket fd to integrate with a poll loop, or -1 if not
 *   connected. Do NOT close this fd.
 */
gint ww_display_get_fd(WwDisplay *self);

/**
 * ww_display_dispatch:
 * @self: a #WwDisplay
 *
 * Drives the steady-state read path. Fires signals synchronously.
 *
 * Returns: number of frames dispatched (>= 0), or negative on failure.
 */
gint ww_display_dispatch(WwDisplay *self);

/**
 * ww_display_update_size:
 * @self: a #WwDisplay
 * @width: new width in pixels
 * @height: new height in pixels
 *
 * Returns: TRUE on success
 */
gboolean ww_display_update_size(WwDisplay *self,
                                guint width,
                                guint height);

/**
 * ww_display_create_gl_texture:
 * @self: a #WwDisplay
 * @image_index: which buffer in the current texture set
 * @out_gl_texture: (out): GL texture name
 *
 * Must be called from a thread with a current GL context bound to the
 * same EGLDisplay passed to ww_display_bind_egl().
 *
 * Returns: TRUE on success
 */
gboolean ww_display_create_gl_texture(WwDisplay *self,
                                      guint image_index,
                                      guint *out_gl_texture);

/**
 * ww_display_delete_gl_texture:
 * @self: a #WwDisplay
 * @image_index: which buffer in the current texture set
 */
void ww_display_delete_gl_texture(WwDisplay *self, guint image_index);

/**
 * ww_display_signal_release_syncobj:
 * @fd: a drm_syncobj fd previously obtained via ww_display_dup_release_fd()
 *
 * Class-level helper. Takes ownership: signals the syncobj and closes
 * @fd on every return path. Use this on a stored (dup'd) fd, NOT on
 * the raw fd value handed to a `frame-ready` handler — the trampoline
 * closes that one for you.
 *
 * Returns: TRUE on success
 */
gboolean ww_display_signal_release_syncobj(gint fd);

/**
 * ww_display_dup_release_fd:
 * @fd: a drm_syncobj fd as received in a `frame-ready` handler
 *
 * Returns a dup of @fd that the caller can store and signal later
 * (e.g. on the next frame_ready, lazy-release pattern). The original
 * is still owned by the C trampoline and will be closed after the
 * handler returns; the dup is owned by the JS caller and must
 * eventually be passed to ww_display_signal_release_syncobj() or
 * ww_display_close_fd().
 *
 * Returns: a fresh fd, or -1 on failure / when @fd is -1.
 */
gint ww_display_dup_release_fd(gint fd);

/**
 * ww_display_close_fd:
 * @fd: a fd previously obtained via ww_display_dup_release_fd()
 *
 * Plain close(2). Use for fd ownership cleanup when dropping a frame
 * without signaling its release (e.g. during shutdown).
 */
void ww_display_close_fd(gint fd);

/**
 * ww_display_disconnect:
 * @self: a #WwDisplay
 */
void ww_display_disconnect(WwDisplay *self);

/**
 * ww_display_conn_state:
 * @self: a #WwDisplay
 *
 * Returns: the current connection state
 */
WwConnState ww_display_conn_state(WwDisplay *self);

/**
 * ww_display_stream_state:
 * @self: a #WwDisplay
 *
 * Returns: the current stream state
 */
WwStreamState ww_display_stream_state(WwDisplay *self);

G_END_DECLS

#endif /* WW_DISPLAY_GOBJECT_H */
