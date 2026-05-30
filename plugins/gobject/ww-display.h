/*
 * libwaywallen-gobject — GObject Introspection wrapper for
 * libwaywallen_display. Exposes namespace `Waywallen` to GJS / Vala.
 *
 * The wrapper deliberately mirrors the C ABI 1:1 with two
 * concessions to GIR ergonomics:
 *
 *   - Backends: only the DMABUF_RELAY backend is exposed — GJS can't
 *     hand the C ABI a raw VkInstance/EGLContext, so the lib owns its
 *     own Vulkan instance and re-exports a LINEAR shadow DMA-BUF that
 *     GTK imports via GdkDmabufTexture.
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

#define WW_TYPE_DISPLAY (ww_display_get_type())
G_DECLARE_FINAL_TYPE(WwDisplay, ww_display, WW, DISPLAY, GObject)

/**
 * ww_display_new:
 *
 * Allocate a fresh WwDisplay. The DMABUF_RELAY backend must be bound via
 * ww_display_bind_dmabuf_relay() before connecting.
 *
 * Returns: (transfer full): a new #WwDisplay
 */
WwDisplay *ww_display_new(void);

/**
 * ww_display_bind_dmabuf_relay:
 * @self: a #WwDisplay
 *
 * Bind the DMABUF_RELAY backend: the library opens a private Vulkan
 * instance/device, imports producer DMA-BUFs, blits into a LINEAR
 * shadow whose DMA-BUF is re-exported for the host to import.
 *
 * Must be called before ww_display_begin_connect(). Useful for shells
 * (GTK4) that can consume DMA-BUFs via GdkDmabufTexture but don't
 * expose a Vulkan device for direct bind_vulkan.
 *
 * Returns: TRUE on success
 */
gboolean ww_display_bind_dmabuf_relay(WwDisplay *self);

/**
 * ww_display_get_shadow_export:
 * @self: a #WwDisplay
 * @out_fd: (out): a fresh dup of the shadow's DMA-BUF fd; the caller
 *   owns it and must close (-1 on failure).
 * @out_n_planes: (out): plane count (always 1 today)
 * @out_strides: (out caller-allocates) (array fixed-size=4): per-plane
 *   row pitch in bytes; only the first @out_n_planes entries are
 *   meaningful.
 * @out_offsets: (out caller-allocates) (array fixed-size=4): per-plane
 *   byte offset into the dmabuf.
 * @out_modifier: (out): DRM format modifier (DRM_FORMAT_MOD_LINEAR=0
 *   today).
 *
 * Read the shadow DMA-BUF descriptor populated on the most recent
 * `textures-ready` for DMABUF_RELAY mode. The fd is dup'd before
 * return so callers can safely close after consuming.
 *
 * Returns: TRUE on success; FALSE if the current backend isn't
 * DMABUF_RELAY or no shadow has been allocated yet.
 */
gboolean ww_display_get_shadow_export(WwDisplay *self,
                                      gint *out_fd,
                                      guint *out_n_planes,
                                      guint out_strides[4],
                                      guint64 out_offsets[4],
                                      guint64 *out_modifier);

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
 * ww_display_close_fd:
 * @fd: a fd received in a `frame-ready` handler
 *
 * Plain close(2). In DMABUF_RELAY mode the lib already signals the
 * release fence internally, so the handler's fd is normally -1; this is
 * defensive cleanup for any non-negative fd handed over.
 */
void ww_display_close_fd(gint fd);

/**
 * ww_display_send_pointer_motion:
 * @self: a #WwDisplay
 * @x: @y: surface-local pixels (same space as the registered width/height)
 * @timestamp_us: monotonic microseconds, or 0 to let the daemon stamp
 * @modifiers: Linux modifier mask, or 0
 *
 * Best-effort forward of a pointer motion to the daemon, which reverse-
 * projects it onto the renderer's texture via the active layout.
 */
void ww_display_send_pointer_motion(WwDisplay *self, gdouble x, gdouble y,
                                    guint64 timestamp_us, guint modifiers);

/**
 * ww_display_send_pointer_button:
 * @self: a #WwDisplay
 * @x: @y: surface-local pixels
 * @button: Linux input code (BTN_LEFT=0x110, BTN_RIGHT=0x111, …)
 * @pressed: TRUE on press, FALSE on release
 * @timestamp_us: monotonic microseconds, or 0
 * @modifiers: Linux modifier mask, or 0
 */
void ww_display_send_pointer_button(WwDisplay *self, gdouble x, gdouble y,
                                    guint button, gboolean pressed,
                                    guint64 timestamp_us, guint modifiers);

/**
 * ww_display_send_pointer_axis:
 * @self: a #WwDisplay
 * @x: @y: surface-local pixels
 * @dx: @dy: scroll deltas in logical notches (wheel)
 * @timestamp_us: monotonic microseconds, or 0
 * @modifiers: Linux modifier mask, or 0
 */
void ww_display_send_pointer_axis(WwDisplay *self, gdouble x, gdouble y,
                                  gdouble dx, gdouble dy,
                                  guint64 timestamp_us, guint modifiers);

/**
 * ww_display_set_window_state:
 * @self: a #WwDisplay
 * @flags: WAYWALLEN_WIN_HAS_* bitmask of windows covering this display
 *   (NON_MINIMIZED=1, ACTIVE=2, MAXIMIZED=4, FULLSCREEN=8)
 *
 * Report covering-window state for autopause. Fire-and-forget — the
 * daemon owns all pause policy; the caller must not debounce or filter.
 */
void ww_display_set_window_state(WwDisplay *self, guint flags);

/**
 * ww_display_disconnect:
 * @self: a #WwDisplay
 */
void ww_display_disconnect(WwDisplay *self);

G_END_DECLS

#endif /* WW_DISPLAY_GOBJECT_H */
