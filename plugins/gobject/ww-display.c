#include "ww-display.h"

#include <waywallen_display.h>

#include <unistd.h>

struct _WwDisplay {
    GObject parent_instance;
    waywallen_display_t *handle;
    gboolean connected;
};

G_DEFINE_FINAL_TYPE(WwDisplay, ww_display, G_TYPE_OBJECT)

enum {
    SIGNAL_TEXTURES_READY,
    SIGNAL_TEXTURES_RELEASING,
    SIGNAL_CONFIG,
    SIGNAL_FRAME_READY,
    SIGNAL_DISCONNECTED,
    LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

static void
on_textures_ready_cb(void *user_data, const waywallen_textures_t *t)
{
    WwDisplay *self = WW_DISPLAY(user_data);
    g_signal_emit(self, signals[SIGNAL_TEXTURES_READY], 0,
                  (guint)t->count,
                  (guint)t->tex_width,
                  (guint)t->tex_height,
                  (guint)t->fourcc,
                  (guint64)t->modifier,
                  (gint)t->backend);
}

static void
on_textures_releasing_cb(void *user_data, const waywallen_textures_t *t)
{
    (void)t;
    WwDisplay *self = WW_DISPLAY(user_data);
    g_signal_emit(self, signals[SIGNAL_TEXTURES_RELEASING], 0);
}

static void
on_config_cb(void *user_data, const waywallen_config_t *c)
{
    /* Forward source/dest rect, transform, and the renderer-published
     * RGBA clear color the daemon supplied via set_config. Consumers
     * MUST treat clear color as authoritative — it's owned by the
     * renderer and there's no display-side knob. */
    WwDisplay *self = WW_DISPLAY(user_data);
    g_signal_emit(self, signals[SIGNAL_CONFIG], 0,
                  (gdouble)c->source_rect.x, (gdouble)c->source_rect.y,
                  (gdouble)c->source_rect.w, (gdouble)c->source_rect.h,
                  (gdouble)c->dest_rect.x,   (gdouble)c->dest_rect.y,
                  (gdouble)c->dest_rect.w,   (gdouble)c->dest_rect.h,
                  (guint)c->transform,
                  (gdouble)c->clear_color[0], (gdouble)c->clear_color[1],
                  (gdouble)c->clear_color[2], (gdouble)c->clear_color[3]);
}

static void
on_frame_ready_cb(void *user_data, const waywallen_frame_t *f)
{
    WwDisplay *self = WW_DISPLAY(user_data);
    gint fd = f->release_syncobj_fd;

    g_signal_emit(self, signals[SIGNAL_FRAME_READY], 0,
                  (guint)f->buffer_index,
                  (guint64)f->seq,
                  fd);

    /* JS handler may have called ww_display_signal_release_syncobj(fd),
     * which operates on a dup. The original fd hand-off semantics are
     * "transfer to host" per the C ABI, so close it here unconditionally. */
    if (fd >= 0)
        close(fd);
}

static void
on_disconnected_cb(void *user_data, int err_code, const char *message)
{
    WwDisplay *self = WW_DISPLAY(user_data);
    self->connected = FALSE;
    g_signal_emit(self, signals[SIGNAL_DISCONNECTED], 0,
                  err_code,
                  message ? message : "");
}

static void
ww_display_finalize(GObject *object)
{
    WwDisplay *self = WW_DISPLAY(object);
    if (self->handle) {
        waywallen_display_disconnect(self->handle);
        waywallen_display_destroy(self->handle);
        self->handle = NULL;
    }
    G_OBJECT_CLASS(ww_display_parent_class)->finalize(object);
}

static void
ww_display_class_init(WwDisplayClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->finalize = ww_display_finalize;

    /* (count, tex_width, tex_height, fourcc, modifier, backend) */
    signals[SIGNAL_TEXTURES_READY] = g_signal_new(
        "textures-ready",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 6,
        G_TYPE_UINT, G_TYPE_UINT, G_TYPE_UINT,
        G_TYPE_UINT, G_TYPE_UINT64, G_TYPE_INT);

    signals[SIGNAL_TEXTURES_RELEASING] = g_signal_new(
        "textures-releasing",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 0);

    /* (src_x, src_y, src_w, src_h,
     *  dst_x, dst_y, dst_w, dst_h,
     *  transform, clear_r, clear_g, clear_b, clear_a) — clear_* is
     * the renderer-published RGBA letterbox color. */
    signals[SIGNAL_CONFIG] = g_signal_new(
        "config",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 13,
        G_TYPE_DOUBLE, G_TYPE_DOUBLE, G_TYPE_DOUBLE, G_TYPE_DOUBLE,
        G_TYPE_DOUBLE, G_TYPE_DOUBLE, G_TYPE_DOUBLE, G_TYPE_DOUBLE,
        G_TYPE_UINT,
        G_TYPE_DOUBLE, G_TYPE_DOUBLE, G_TYPE_DOUBLE, G_TYPE_DOUBLE);

    /* (buffer_index, seq, release_syncobj_fd) */
    signals[SIGNAL_FRAME_READY] = g_signal_new(
        "frame-ready",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 3,
        G_TYPE_UINT, G_TYPE_UINT64, G_TYPE_INT);

    /* (err_code, message) */
    signals[SIGNAL_DISCONNECTED] = g_signal_new(
        "disconnected",
        G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST,
        0, NULL, NULL, NULL,
        G_TYPE_NONE, 2,
        G_TYPE_INT, G_TYPE_STRING);
}

static void
ww_display_init(WwDisplay *self)
{
    waywallen_display_callbacks_t cb = {
        .on_textures_ready     = on_textures_ready_cb,
        .on_textures_releasing = on_textures_releasing_cb,
        .on_config             = on_config_cb,
        .on_frame_ready        = on_frame_ready_cb,
        .on_disconnected       = on_disconnected_cb,
        .user_data             = self,
    };
    self->handle = waywallen_display_new(&cb);
    self->connected = FALSE;
}

WwDisplay *
ww_display_new(void)
{
    return g_object_new(WW_TYPE_DISPLAY, NULL);
}

gboolean
ww_display_bind_egl(WwDisplay *self,
                    gpointer egl_display,
                    gpointer get_proc_address)
{
    g_return_val_if_fail(WW_IS_DISPLAY(self), FALSE);
    g_return_val_if_fail(self->handle != NULL, FALSE);

    waywallen_egl_ctx_t ctx = {
        .egl_display = egl_display,
        .get_proc_address = (void *(*)(const char *))get_proc_address,
    };
    return waywallen_display_bind_egl(self->handle, &ctx) == WAYWALLEN_OK;
}

gboolean
ww_display_set_drm_render_node(WwDisplay *self, guint major, guint minor)
{
    g_return_val_if_fail(WW_IS_DISPLAY(self), FALSE);
    return waywallen_display_set_drm_render_node(self->handle,
                                                 (uint32_t)major,
                                                 (uint32_t)minor)
           == WAYWALLEN_OK;
}

gboolean
ww_display_begin_connect(WwDisplay *self,
                         const gchar *socket_path,
                         const gchar *display_name,
                         const gchar *instance_id,
                         guint width,
                         guint height,
                         guint refresh_mhz)
{
    g_return_val_if_fail(WW_IS_DISPLAY(self), FALSE);
    g_return_val_if_fail(display_name != NULL, FALSE);

    int rc = waywallen_display_begin_connect_v2(
        self->handle,
        socket_path,
        display_name,
        instance_id,
        (uint32_t)width,
        (uint32_t)height,
        (uint32_t)refresh_mhz);
    if (rc == WAYWALLEN_OK) {
        self->connected = TRUE;
        return TRUE;
    }
    return FALSE;
}

gint
ww_display_advance_handshake(WwDisplay *self)
{
    g_return_val_if_fail(WW_IS_DISPLAY(self), -1);
    return waywallen_display_advance_handshake(self->handle);
}

WwHandshakeState
ww_display_handshake_state(WwDisplay *self)
{
    g_return_val_if_fail(WW_IS_DISPLAY(self), WW_HANDSHAKE_STATE_IDLE);
    return (WwHandshakeState)waywallen_display_handshake_state(self->handle);
}

gint
ww_display_get_fd(WwDisplay *self)
{
    g_return_val_if_fail(WW_IS_DISPLAY(self), -1);
    return waywallen_display_get_fd(self->handle);
}

gint
ww_display_dispatch(WwDisplay *self)
{
    g_return_val_if_fail(WW_IS_DISPLAY(self), -1);
    return waywallen_display_dispatch(self->handle);
}

gboolean
ww_display_update_size(WwDisplay *self, guint width, guint height)
{
    g_return_val_if_fail(WW_IS_DISPLAY(self), FALSE);
    return waywallen_display_update_size(self->handle,
                                         (uint32_t)width,
                                         (uint32_t)height)
           == WAYWALLEN_OK;
}

gboolean
ww_display_create_gl_texture(WwDisplay *self,
                             guint image_index,
                             guint *out_gl_texture)
{
    g_return_val_if_fail(WW_IS_DISPLAY(self), FALSE);
    g_return_val_if_fail(out_gl_texture != NULL, FALSE);

    uint32_t tex = 0;
    int rc = waywallen_display_create_gl_texture(self->handle,
                                                 (uint32_t)image_index,
                                                 &tex);
    if (rc != WAYWALLEN_OK)
        return FALSE;
    *out_gl_texture = (guint)tex;
    return TRUE;
}

void
ww_display_delete_gl_texture(WwDisplay *self, guint image_index)
{
    g_return_if_fail(WW_IS_DISPLAY(self));
    waywallen_display_delete_gl_texture(self->handle, (uint32_t)image_index);
}

gboolean
ww_display_signal_release_syncobj(gint fd)
{
    if (fd < 0)
        return FALSE;
    /* Pure ownership-transfer: the C ABI helper closes @fd on every path. */
    return waywallen_display_signal_release_syncobj(fd) == WAYWALLEN_OK;
}

gint
ww_display_dup_release_fd(gint fd)
{
    if (fd < 0)
        return -1;
    return dup(fd);
}

void
ww_display_close_fd(gint fd)
{
    if (fd >= 0)
        close(fd);
}

void
ww_display_disconnect(WwDisplay *self)
{
    g_return_if_fail(WW_IS_DISPLAY(self));
    waywallen_display_disconnect(self->handle);
    self->connected = FALSE;
}

WwConnState
ww_display_conn_state(WwDisplay *self)
{
    g_return_val_if_fail(WW_IS_DISPLAY(self), WW_CONN_STATE_DISCONNECTED);
    return (WwConnState)waywallen_display_conn_state(self->handle);
}

WwStreamState
ww_display_stream_state(WwDisplay *self)
{
    g_return_val_if_fail(WW_IS_DISPLAY(self), WW_STREAM_STATE_INACTIVE);
    return (WwStreamState)waywallen_display_stream_state(self->handle);
}
