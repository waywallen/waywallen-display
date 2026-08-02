#include "ww-display.h"

#include <waywallen_display.h>

#include <unistd.h>

struct _WwDisplay {
    GObject              parent_instance;
    waywallen_display_t* handle;
    gboolean             connected;
    /* DMABUF_RELAY: snapshot of the most recent binding's
     * shadow descriptor. Filled in the trampoline; consumers read via
     * ww_display_get_shadow_export. fd is lib-owned — wrapper dups on
     * the way out. */
    gint              shadow_fd;
    guint             shadow_n_planes;
    guint             shadow_strides[4];
    guint64           shadow_offsets[4];
    guint64           shadow_modifier;
    gboolean          shadow_valid;
    WwPauseEffectKind pause_effect_kind;
    guint             blur_radius;
    gboolean          pause_effect_active;
    guint64           presentation_config_generation;
    guint64           presentation_state_generation;
};

G_DEFINE_FINAL_TYPE(WwDisplay, ww_display, G_TYPE_OBJECT)

enum
{
    SIGNAL_BINDING_READY,
    SIGNAL_TEXTURES_RELEASING,
    SIGNAL_COMPOSITION_CONFIG,
    SIGNAL_FRAME_READY,
    SIGNAL_PRESENTATION_SNAPSHOT,
    SIGNAL_PRESENTATION_STATE,
    SIGNAL_DISCONNECTED,
    LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

enum
{
    PROP_0,
    PROP_PAUSE_EFFECT_KIND,
    PROP_BLUR_RADIUS,
    PROP_PAUSE_EFFECT_ACTIVE,
    PROP_PRESENTATION_CONFIG_GENERATION,
    PROP_PRESENTATION_STATE_GENERATION,
    LAST_PROPERTY,
};

static GParamSpec* properties[LAST_PROPERTY] = { NULL };

static void notify_presentation(WwDisplay* self) {
    g_object_freeze_notify(G_OBJECT(self));
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_PAUSE_EFFECT_KIND]);
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_BLUR_RADIUS]);
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_PAUSE_EFFECT_ACTIVE]);
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_PRESENTATION_CONFIG_GENERATION]);
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_PRESENTATION_STATE_GENERATION]);
    g_object_thaw_notify(G_OBJECT(self));
}

static void on_binding_ready_cb(void* user_data, const waywallen_binding_t* binding) {
    WwDisplay*                            self = WW_DISPLAY(user_data);
    const waywallen_textures_t*           t    = &binding->textures;
    const waywallen_composition_config_t* c    = &binding->config;
    /* Snapshot the shadow descriptor while the textures_t is still
     * the lib's live copy. Always refresh — relay rebinds change the
     * shadow fd. */
    self->shadow_valid    = FALSE;
    self->shadow_fd       = t->shadow_dmabuf_fd;
    self->shadow_n_planes = (guint)t->shadow_n_planes;
    self->shadow_modifier = t->shadow_modifier;
    for (guint i = 0; i < 4; i++) {
        self->shadow_strides[i] = (i < t->shadow_n_planes) ? (guint)t->shadow_strides[i] : 0u;
        self->shadow_offsets[i] = (i < t->shadow_n_planes) ? t->shadow_offsets[i] : 0ull;
    }
    if (t->shadow_dmabuf_fd >= 0 && t->shadow_n_planes > 0) {
        self->shadow_valid = TRUE;
    }
    g_signal_emit(self,
                  signals[SIGNAL_BINDING_READY],
                  0,
                  (guint)t->count,
                  (guint)t->tex_width,
                  (guint)t->tex_height,
                  (guint)t->fourcc,
                  (guint64)t->modifier,
                  (gint)t->backend,
                  (gdouble)c->source_rect.x,
                  (gdouble)c->source_rect.y,
                  (gdouble)c->source_rect.w,
                  (gdouble)c->source_rect.h,
                  (gdouble)c->dest_rect.x,
                  (gdouble)c->dest_rect.y,
                  (gdouble)c->dest_rect.w,
                  (gdouble)c->dest_rect.h,
                  (guint)c->transform,
                  (gdouble)c->clear_color.r,
                  (gdouble)c->clear_color.g,
                  (gdouble)c->clear_color.b,
                  (gdouble)c->clear_color.a);
}

static void on_textures_releasing_cb(void* user_data, const waywallen_textures_t* t) {
    (void)t;
    WwDisplay* self       = WW_DISPLAY(user_data);
    self->shadow_valid    = FALSE;
    self->shadow_fd       = -1;
    self->shadow_n_planes = 0;
    g_signal_emit(self, signals[SIGNAL_TEXTURES_RELEASING], 0);
}

static void on_composition_config_cb(void* user_data, const waywallen_composition_config_t* c) {
    /* Forward source/dest rect, transform, and the renderer-published
     * RGBA clear color the daemon supplied. Consumers
     * MUST treat clear color as authoritative — it's owned by the
     * renderer and there's no display-side knob. */
    WwDisplay* self = WW_DISPLAY(user_data);
    g_signal_emit(self,
                  signals[SIGNAL_COMPOSITION_CONFIG],
                  0,
                  (gdouble)c->source_rect.x,
                  (gdouble)c->source_rect.y,
                  (gdouble)c->source_rect.w,
                  (gdouble)c->source_rect.h,
                  (gdouble)c->dest_rect.x,
                  (gdouble)c->dest_rect.y,
                  (gdouble)c->dest_rect.w,
                  (gdouble)c->dest_rect.h,
                  (guint)c->transform,
                  (gdouble)c->clear_color.r,
                  (gdouble)c->clear_color.g,
                  (gdouble)c->clear_color.b,
                  (gdouble)c->clear_color.a);
}

static void on_frame_ready_cb(void* user_data, const waywallen_frame_t* f) {
    WwDisplay* self = WW_DISPLAY(user_data);
    gint       fd   = f->release_syncobj_fd;

    g_signal_emit(
        self, signals[SIGNAL_FRAME_READY], 0, (guint)f->buffer_index, (guint64)f->seq, fd);

    /* JS handler may have called ww_display_signal_release_syncobj(fd),
     * which operates on a dup. The original fd hand-off semantics are
     * "transfer to host" per the C ABI, so close it here unconditionally. */
    if (fd >= 0) close(fd);
}

static void on_presentation_snapshot_cb(void*                                    user_data,
                                        const waywallen_presentation_snapshot_t* presentation) {
    WwDisplay* self           = WW_DISPLAY(user_data);
    self->pause_effect_kind   = (WwPauseEffectKind)presentation->config.pause_effect.kind;
    self->blur_radius         = presentation->config.pause_effect.blur.radius;
    self->pause_effect_active = presentation->state.pause_effect.active;
    self->presentation_config_generation = presentation->config.generation;
    self->presentation_state_generation  = presentation->state.generation;
    notify_presentation(self);
    g_signal_emit(self,
                  signals[SIGNAL_PRESENTATION_SNAPSHOT],
                  0,
                  self->presentation_config_generation,
                  self->presentation_state_generation,
                  self->pause_effect_kind,
                  self->blur_radius,
                  self->pause_effect_active);
}

static void on_presentation_state_cb(void* user_data, const waywallen_presentation_state_t* state) {
    WwDisplay* self                     = WW_DISPLAY(user_data);
    self->pause_effect_active           = state->pause_effect.active;
    self->presentation_state_generation = state->generation;
    notify_presentation(self);
    g_signal_emit(self,
                  signals[SIGNAL_PRESENTATION_STATE],
                  0,
                  state->generation,
                  state->config_generation,
                  state->pause_effect.active);
}

static void on_disconnected_cb(void* user_data, int err_code, const char* message) {
    WwDisplay* self = WW_DISPLAY(user_data);
    self->connected = FALSE;
    g_signal_emit(self, signals[SIGNAL_DISCONNECTED], 0, err_code, message ? message : "");
}

static void ww_display_finalize(GObject* object) {
    WwDisplay* self = WW_DISPLAY(object);
    if (self->handle) {
        waywallen_display_shutdown(self->handle);
        self->handle = NULL;
    }
    G_OBJECT_CLASS(ww_display_parent_class)->finalize(object);
}

static void ww_display_get_property(GObject* object, guint property_id, GValue* value,
                                    GParamSpec* pspec) {
    WwDisplay* self = WW_DISPLAY(object);
    switch (property_id) {
    case PROP_PAUSE_EFFECT_KIND: g_value_set_uint(value, self->pause_effect_kind); break;
    case PROP_BLUR_RADIUS: g_value_set_uint(value, self->blur_radius); break;
    case PROP_PAUSE_EFFECT_ACTIVE: g_value_set_boolean(value, self->pause_effect_active); break;
    case PROP_PRESENTATION_CONFIG_GENERATION:
        g_value_set_uint64(value, self->presentation_config_generation);
        break;
    case PROP_PRESENTATION_STATE_GENERATION:
        g_value_set_uint64(value, self->presentation_state_generation);
        break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec); break;
    }
}

static void ww_display_class_init(WwDisplayClass* klass) {
    GObjectClass* object_class = G_OBJECT_CLASS(klass);
    object_class->finalize     = ww_display_finalize;
    object_class->get_property = ww_display_get_property;

    properties[PROP_PAUSE_EFFECT_KIND] =
        g_param_spec_uint("pause-effect-kind",
                          "Pause effect kind",
                          "Configured effect kind for a paused renderer",
                          WW_PAUSE_EFFECT_KIND_NONE,
                          WW_PAUSE_EFFECT_KIND_BLUR,
                          WW_PAUSE_EFFECT_KIND_NONE,
                          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    properties[PROP_BLUR_RADIUS] = g_param_spec_uint("blur-radius",
                                                     "Blur radius",
                                                     "Configured logical blur radius",
                                                     1,
                                                     64,
                                                     30,
                                                     G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    properties[PROP_PAUSE_EFFECT_ACTIVE] =
        g_param_spec_boolean("pause-effect-active",
                             "Pause effect active",
                             "Whether the configured pause effect is active",
                             FALSE,
                             G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    properties[PROP_PRESENTATION_CONFIG_GENERATION] =
        g_param_spec_uint64("presentation-config-generation",
                            "Presentation config generation",
                            "Connection-local persistent presentation generation",
                            0,
                            G_MAXUINT64,
                            0,
                            G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    properties[PROP_PRESENTATION_STATE_GENERATION] =
        g_param_spec_uint64("presentation-state-generation",
                            "Presentation state generation",
                            "Connection-local runtime presentation state generation",
                            0,
                            G_MAXUINT64,
                            0,
                            G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
    g_object_class_install_properties(object_class, LAST_PROPERTY, properties);

    /* Texture metadata and its initial composition are one snapshot. */
    signals[SIGNAL_BINDING_READY] = g_signal_new("binding-ready",
                                                 G_TYPE_FROM_CLASS(klass),
                                                 G_SIGNAL_RUN_LAST,
                                                 0,
                                                 NULL,
                                                 NULL,
                                                 NULL,
                                                 G_TYPE_NONE,
                                                 19,
                                                 G_TYPE_UINT,
                                                 G_TYPE_UINT,
                                                 G_TYPE_UINT,
                                                 G_TYPE_UINT,
                                                 G_TYPE_UINT64,
                                                 G_TYPE_INT,
                                                 G_TYPE_DOUBLE,
                                                 G_TYPE_DOUBLE,
                                                 G_TYPE_DOUBLE,
                                                 G_TYPE_DOUBLE,
                                                 G_TYPE_DOUBLE,
                                                 G_TYPE_DOUBLE,
                                                 G_TYPE_DOUBLE,
                                                 G_TYPE_DOUBLE,
                                                 G_TYPE_UINT,
                                                 G_TYPE_DOUBLE,
                                                 G_TYPE_DOUBLE,
                                                 G_TYPE_DOUBLE,
                                                 G_TYPE_DOUBLE);

    signals[SIGNAL_TEXTURES_RELEASING] = g_signal_new("textures-releasing",
                                                      G_TYPE_FROM_CLASS(klass),
                                                      G_SIGNAL_RUN_LAST,
                                                      0,
                                                      NULL,
                                                      NULL,
                                                      NULL,
                                                      G_TYPE_NONE,
                                                      0);

    /* (src_x, src_y, src_w, src_h,
     *  dst_x, dst_y, dst_w, dst_h,
     *  transform, clear_r, clear_g, clear_b, clear_a) — clear_* is
     * the renderer-published RGBA letterbox color. */
    signals[SIGNAL_COMPOSITION_CONFIG] = g_signal_new("composition-config",
                                                      G_TYPE_FROM_CLASS(klass),
                                                      G_SIGNAL_RUN_LAST,
                                                      0,
                                                      NULL,
                                                      NULL,
                                                      NULL,
                                                      G_TYPE_NONE,
                                                      13,
                                                      G_TYPE_DOUBLE,
                                                      G_TYPE_DOUBLE,
                                                      G_TYPE_DOUBLE,
                                                      G_TYPE_DOUBLE,
                                                      G_TYPE_DOUBLE,
                                                      G_TYPE_DOUBLE,
                                                      G_TYPE_DOUBLE,
                                                      G_TYPE_DOUBLE,
                                                      G_TYPE_UINT,
                                                      G_TYPE_DOUBLE,
                                                      G_TYPE_DOUBLE,
                                                      G_TYPE_DOUBLE,
                                                      G_TYPE_DOUBLE);

    /* (buffer_index, seq, release_syncobj_fd) */
    signals[SIGNAL_FRAME_READY] = g_signal_new("frame-ready",
                                               G_TYPE_FROM_CLASS(klass),
                                               G_SIGNAL_RUN_LAST,
                                               0,
                                               NULL,
                                               NULL,
                                               NULL,
                                               G_TYPE_NONE,
                                               3,
                                               G_TYPE_UINT,
                                               G_TYPE_UINT64,
                                               G_TYPE_INT);

    /* (config_generation, state_generation, kind, radius, active) */
    signals[SIGNAL_PRESENTATION_SNAPSHOT] = g_signal_new("presentation-snapshot",
                                                         G_TYPE_FROM_CLASS(klass),
                                                         G_SIGNAL_RUN_LAST,
                                                         0,
                                                         NULL,
                                                         NULL,
                                                         NULL,
                                                         G_TYPE_NONE,
                                                         5,
                                                         G_TYPE_UINT64,
                                                         G_TYPE_UINT64,
                                                         G_TYPE_UINT,
                                                         G_TYPE_UINT,
                                                         G_TYPE_BOOLEAN);

    /* (state_generation, config_generation, active) */
    signals[SIGNAL_PRESENTATION_STATE] = g_signal_new("presentation-state",
                                                      G_TYPE_FROM_CLASS(klass),
                                                      G_SIGNAL_RUN_LAST,
                                                      0,
                                                      NULL,
                                                      NULL,
                                                      NULL,
                                                      G_TYPE_NONE,
                                                      3,
                                                      G_TYPE_UINT64,
                                                      G_TYPE_UINT64,
                                                      G_TYPE_BOOLEAN);

    /* (err_code, message) */
    signals[SIGNAL_DISCONNECTED] = g_signal_new("disconnected",
                                                G_TYPE_FROM_CLASS(klass),
                                                G_SIGNAL_RUN_LAST,
                                                0,
                                                NULL,
                                                NULL,
                                                NULL,
                                                G_TYPE_NONE,
                                                2,
                                                G_TYPE_INT,
                                                G_TYPE_STRING);
}

static void ww_display_init(WwDisplay* self) {
    waywallen_display_callbacks_t cb = {
        .on_binding_ready         = on_binding_ready_cb,
        .on_textures_releasing    = on_textures_releasing_cb,
        .on_composition_config    = on_composition_config_cb,
        .on_frame_ready           = on_frame_ready_cb,
        .on_presentation_snapshot = on_presentation_snapshot_cb,
        .on_presentation_state    = on_presentation_state_cb,
        .on_disconnected          = on_disconnected_cb,
        .user_data                = self,
    };
    self->handle            = waywallen_display_new(&cb);
    self->connected         = FALSE;
    self->shadow_fd         = -1;
    self->shadow_n_planes   = 0;
    self->shadow_modifier   = 0;
    self->shadow_valid      = FALSE;
    self->pause_effect_kind = WW_PAUSE_EFFECT_KIND_NONE;
    self->blur_radius       = 30;
}

WwDisplay* ww_display_new(void) { return g_object_new(WW_TYPE_DISPLAY, NULL); }

gboolean ww_display_bind_dmabuf_relay(WwDisplay* self) {
    g_return_val_if_fail(WW_IS_DISPLAY(self), FALSE);
    g_return_val_if_fail(self->handle != NULL, FALSE);
    return waywallen_display_bind_dmabuf_relay(self->handle) == WAYWALLEN_OK;
}

gboolean ww_display_set_presentation_capabilities(WwDisplay* self, guint flags) {
    g_return_val_if_fail(WW_IS_DISPLAY(self), FALSE);
    g_return_val_if_fail(self->handle != NULL, FALSE);
    return waywallen_display_set_presentation_caps(self->handle, flags) == WAYWALLEN_OK;
}

gboolean ww_display_get_shadow_export(WwDisplay* self, gint* out_fd, guint* out_n_planes,
                                      guint out_strides[4], guint64 out_offsets[4],
                                      guint64* out_modifier) {
    g_return_val_if_fail(WW_IS_DISPLAY(self), FALSE);
    g_return_val_if_fail(out_fd != NULL, FALSE);
    g_return_val_if_fail(out_n_planes != NULL, FALSE);
    g_return_val_if_fail(out_strides != NULL, FALSE);
    g_return_val_if_fail(out_offsets != NULL, FALSE);
    g_return_val_if_fail(out_modifier != NULL, FALSE);
    if (! self->shadow_valid || self->shadow_fd < 0) {
        *out_fd       = -1;
        *out_n_planes = 0;
        *out_modifier = 0;
        for (guint i = 0; i < 4; i++) {
            out_strides[i] = 0;
            out_offsets[i] = 0;
        }
        return FALSE;
    }
    /* dup so callers get an fd they own. */
    int dup_fd = dup(self->shadow_fd);
    if (dup_fd < 0) {
        *out_fd = -1;
        return FALSE;
    }
    *out_fd       = dup_fd;
    *out_n_planes = self->shadow_n_planes;
    *out_modifier = self->shadow_modifier;
    for (guint i = 0; i < 4; i++) {
        out_strides[i] = self->shadow_strides[i];
        out_offsets[i] = self->shadow_offsets[i];
    }
    return TRUE;
}

gboolean ww_display_begin_connect(WwDisplay* self, const gchar* socket_path,
                                  const gchar* display_name, const gchar* instance_id, guint width,
                                  guint height, guint refresh_mhz) {
    g_return_val_if_fail(WW_IS_DISPLAY(self), FALSE);
    g_return_val_if_fail(display_name != NULL, FALSE);

    const waywallen_display_metrics_t metrics = {
        .width       = (uint32_t)width,
        .height      = (uint32_t)height,
        .refresh_mhz = (uint32_t)refresh_mhz,
    };
    int rc = waywallen_display_begin_connect(
        self->handle, socket_path, display_name, instance_id, &metrics);
    if (rc == WAYWALLEN_OK) {
        self->connected = TRUE;
        return TRUE;
    }
    return FALSE;
}

gint ww_display_advance_handshake(WwDisplay* self) {
    g_return_val_if_fail(WW_IS_DISPLAY(self), -1);
    return waywallen_display_advance_handshake(self->handle);
}

WwHandshakeState ww_display_handshake_state(WwDisplay* self) {
    g_return_val_if_fail(WW_IS_DISPLAY(self), WW_HANDSHAKE_STATE_IDLE);
    return (WwHandshakeState)waywallen_display_handshake_state(self->handle);
}

gint ww_display_get_fd(WwDisplay* self) {
    g_return_val_if_fail(WW_IS_DISPLAY(self), -1);
    return waywallen_display_get_fd(self->handle);
}

gint ww_display_dispatch(WwDisplay* self) {
    g_return_val_if_fail(WW_IS_DISPLAY(self), -1);
    return waywallen_display_dispatch(self->handle);
}

gboolean ww_display_set_metrics(WwDisplay* self, guint width, guint height, guint refresh_mhz) {
    g_return_val_if_fail(WW_IS_DISPLAY(self), FALSE);
    const waywallen_display_metrics_t metrics = {
        .width       = (uint32_t)width,
        .height      = (uint32_t)height,
        .refresh_mhz = (uint32_t)refresh_mhz,
    };
    return waywallen_display_set_metrics(self->handle, &metrics) == WAYWALLEN_OK;
}

void ww_display_close_fd(gint fd) {
    if (fd >= 0) close(fd);
}

void ww_display_send_pointer_motion(WwDisplay* self, gdouble x, gdouble y, guint64 timestamp_us,
                                    guint modifiers) {
    if (! self->handle) return;
    (void)waywallen_display_send_pointer_motion(
        self->handle, (float)x, (float)y, timestamp_us, modifiers);
}

void ww_display_send_pointer_button(WwDisplay* self, gdouble x, gdouble y, guint button,
                                    gboolean pressed, guint64 timestamp_us, guint modifiers) {
    if (! self->handle) return;
    (void)waywallen_display_send_pointer_button(self->handle,
                                                (float)x,
                                                (float)y,
                                                button,
                                                pressed ? WAYWALLEN_POINTER_BUTTON_STATE_PRESSED
                                                        : WAYWALLEN_POINTER_BUTTON_STATE_RELEASED,
                                                timestamp_us,
                                                modifiers);
}

void ww_display_send_pointer_axis(WwDisplay* self, gdouble x, gdouble y, gdouble dx, gdouble dy,
                                  guint64 timestamp_us, guint modifiers) {
    if (! self->handle) return;
    (void)waywallen_display_send_pointer_axis(self->handle,
                                              (float)x,
                                              (float)y,
                                              (float)dx,
                                              (float)dy,
                                              WAYWALLEN_POINTER_AXIS_SOURCE_WHEEL,
                                              timestamp_us,
                                              modifiers);
}

void ww_display_set_window_state(WwDisplay* self, guint flags) {
    if (! self->handle) return;
    (void)waywallen_display_set_window_state(self->handle, flags);
}

void ww_display_disconnect(WwDisplay* self) {
    g_return_if_fail(WW_IS_DISPLAY(self));
    waywallen_display_close(self->handle);
    self->connected = FALSE;
    if (self->pause_effect_kind != WW_PAUSE_EFFECT_KIND_NONE || self->pause_effect_active ||
        self->blur_radius != 30 || self->presentation_config_generation != 0 ||
        self->presentation_state_generation != 0) {
        self->pause_effect_kind              = WW_PAUSE_EFFECT_KIND_NONE;
        self->blur_radius                    = 30;
        self->pause_effect_active            = FALSE;
        self->presentation_config_generation = 0;
        self->presentation_state_generation  = 0;
        notify_presentation(self);
        g_signal_emit(self,
                      signals[SIGNAL_PRESENTATION_SNAPSHOT],
                      0,
                      0,
                      0,
                      WW_PAUSE_EFFECT_KIND_NONE,
                      30u,
                      FALSE);
    }
}
