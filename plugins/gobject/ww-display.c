#include "ww-display-private.h"

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
    gint                    shadow_fd;
    guint                   shadow_n_planes;
    guint                   shadow_strides[4];
    guint64                 shadow_offsets[4];
    guint64                 shadow_modifier;
    guint64                 shadow_buffer_generation;
    gboolean                shadow_valid;
    WwPauseEffectKind       pause_effect_kind;
    guint                   blur_radius;
    gboolean                pause_effect_active;
    guint64                 presentation_config_generation;
    guint64                 presentation_state_generation;
    WwTransitionKind        transition_kind;
    guint                   transition_duration_ms;
    guint                   transition_angle;
    gdouble                 transition_origin_x;
    gdouble                 transition_origin_y;
    WwDisplayNativeListener native_listener;
    void*                   native_listener_data;
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
    SIGNAL_WINDOW_OBSERVATION_CONFIG,
    SIGNAL_WINDOW_OBSERVATION_RULES,
    SIGNAL_PREFERRED_RENDER_DEVICE,
    LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

static void on_preferred_render_device(void* data, uint32_t major, uint32_t minor) {
    g_signal_emit(WW_DISPLAY(data), signals[SIGNAL_PREFERRED_RENDER_DEVICE], 0, major, minor);
}

static void on_window_observation_config(void*                                        data,
                                         const waywallen_window_observation_config_t* config) {
    WwDisplay* self           = WW_DISPLAY(data);
    gchar**    ids            = g_new0(gchar*, config->excluded_application_ids.count + 1);
    gchar**    titles         = g_new0(gchar*, config->excluded_titles.count + 1);
    gchar**    id_patterns    = g_new0(gchar*, config->excluded_application_id_patterns.count + 1);
    gchar**    title_patterns = g_new0(gchar*, config->excluded_title_patterns.count + 1);
    for (guint i = 0; i < config->excluded_application_ids.count; ++i)
        ids[i] = config->excluded_application_ids.data[i];
    for (guint i = 0; i < config->excluded_titles.count; ++i)
        titles[i] = config->excluded_titles.data[i];
    for (guint i = 0; i < config->excluded_application_id_patterns.count; ++i)
        id_patterns[i] = config->excluded_application_id_patterns.data[i];
    for (guint i = 0; i < config->excluded_title_patterns.count; ++i)
        title_patterns[i] = config->excluded_title_patterns.data[i];
    g_signal_emit(self,
                  signals[SIGNAL_WINDOW_OBSERVATION_RULES],
                  0,
                  config->generation,
                  ids,
                  titles,
                  id_patterns,
                  title_patterns);
    g_signal_emit(
        self, signals[SIGNAL_WINDOW_OBSERVATION_CONFIG], 0, config->generation, ids, titles);
    g_free(ids);
    g_free(titles);
    g_free(id_patterns);
    g_free(title_patterns);
}

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
    self->shadow_valid             = FALSE;
    self->shadow_fd                = t->shadow_dmabuf_fd;
    self->shadow_n_planes          = (guint)t->shadow_n_planes;
    self->shadow_modifier          = t->shadow_modifier;
    self->shadow_buffer_generation = t->buffer_generation;
    for (guint i = 0; i < 4; i++) {
        self->shadow_strides[i] = (i < t->shadow_n_planes) ? (guint)t->shadow_strides[i] : 0u;
        self->shadow_offsets[i] = (i < t->shadow_n_planes) ? t->shadow_offsets[i] : 0ull;
    }
    if (t->shadow_dmabuf_fd >= 0 && t->shadow_n_planes > 0) {
        self->shadow_valid = TRUE;
    }
    if (self->native_listener.binding_ready) {
        self->native_listener.binding_ready(self, binding, self->native_listener_data);
    }
    g_signal_emit(self,
                  signals[SIGNAL_BINDING_READY],
                  0,
                  (guint64)t->buffer_generation,
                  (guint64)binding->content_token,
                  (guint64)binding->presentation_config_generation,
                  (guint64)c->generation,
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
    WwDisplay* self = WW_DISPLAY(user_data);
    if (self->native_listener.textures_releasing) {
        self->native_listener.textures_releasing(self, t, self->native_listener_data);
    }
    if (self->shadow_buffer_generation == t->buffer_generation) {
        self->shadow_valid             = FALSE;
        self->shadow_fd                = -1;
        self->shadow_n_planes          = 0;
        self->shadow_buffer_generation = 0;
    }
    g_signal_emit(self, signals[SIGNAL_TEXTURES_RELEASING], 0, (guint64)t->buffer_generation);
}

static void on_composition_config_cb(void* user_data, const waywallen_composition_config_t* c) {
    /* Forward source/dest rect, transform, and the renderer-published
     * RGBA clear color the daemon supplied. Consumers
     * MUST treat clear color as authoritative — it's owned by the
     * renderer and there's no display-side knob. */
    WwDisplay* self = WW_DISPLAY(user_data);
    if (self->native_listener.composition_config) {
        self->native_listener.composition_config(self, c, self->native_listener_data);
    }
    g_signal_emit(self,
                  signals[SIGNAL_COMPOSITION_CONFIG],
                  0,
                  (guint64)c->generation,
                  (guint64)c->buffer_generation,
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

    if (self->native_listener.frame_ready) {
        waywallen_frame_t frame = *f;
        if (frame.release_syncobj_fd >= 0) frame.release_syncobj_fd = dup(frame.release_syncobj_fd);
        self->native_listener.frame_ready(self, &frame, self->native_listener_data);
    }

    g_signal_emit(self,
                  signals[SIGNAL_FRAME_READY],
                  0,
                  (guint64)f->buffer_generation,
                  (guint)f->buffer_index,
                  (guint64)f->seq,
                  fd);

    /* The signal preserves the C callback's transfer-to-host contract.
     * The synchronous handler owns any non-negative fd. */
}

static void on_presentation_snapshot_cb(void*                                    user_data,
                                        const waywallen_presentation_snapshot_t* presentation) {
    WwDisplay* self           = WW_DISPLAY(user_data);
    self->pause_effect_kind   = (WwPauseEffectKind)presentation->config.pause_effect.kind;
    self->blur_radius         = presentation->config.pause_effect.blur.radius;
    self->pause_effect_active = presentation->state.pause_effect.active;
    self->presentation_config_generation = presentation->config.generation;
    self->presentation_state_generation  = presentation->state.generation;
    self->transition_kind                = (WwTransitionKind)presentation->config.transition.kind;
    self->transition_duration_ms         = presentation->config.transition.duration_ms;
    self->transition_angle               = presentation->config.transition.angle;
    self->transition_origin_x            = presentation->config.transition.origin_x;
    self->transition_origin_y            = presentation->config.transition.origin_y;
    notify_presentation(self);
    if (self->native_listener.presentation_snapshot) {
        self->native_listener.presentation_snapshot(self, presentation, self->native_listener_data);
    }
    g_signal_emit(self,
                  signals[SIGNAL_PRESENTATION_SNAPSHOT],
                  0,
                  self->presentation_config_generation,
                  self->presentation_state_generation,
                  self->pause_effect_kind,
                  self->blur_radius,
                  self->pause_effect_active,
                  self->transition_kind,
                  self->transition_duration_ms,
                  self->transition_angle,
                  self->transition_origin_x,
                  self->transition_origin_y);
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
                                                 23,
                                                 G_TYPE_UINT64,
                                                 G_TYPE_UINT64,
                                                 G_TYPE_UINT64,
                                                 G_TYPE_UINT64,
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
                                                      1,
                                                      G_TYPE_UINT64);

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
                                                      15,
                                                      G_TYPE_UINT64,
                                                      G_TYPE_UINT64,
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

    /* (buffer_generation, buffer_index, seq, release_syncobj_fd) */
    signals[SIGNAL_FRAME_READY] = g_signal_new("frame-ready",
                                               G_TYPE_FROM_CLASS(klass),
                                               G_SIGNAL_RUN_LAST,
                                               0,
                                               NULL,
                                               NULL,
                                               NULL,
                                               G_TYPE_NONE,
                                               4,
                                               G_TYPE_UINT64,
                                               G_TYPE_UINT,
                                               G_TYPE_UINT64,
                                               G_TYPE_INT);

    /* (config_generation, state_generation, pause kind/radius/active,
     *  transition kind/duration/angle/origin) */
    signals[SIGNAL_PRESENTATION_SNAPSHOT] = g_signal_new("presentation-snapshot",
                                                         G_TYPE_FROM_CLASS(klass),
                                                         G_SIGNAL_RUN_LAST,
                                                         0,
                                                         NULL,
                                                         NULL,
                                                         NULL,
                                                         G_TYPE_NONE,
                                                         10,
                                                         G_TYPE_UINT64,
                                                         G_TYPE_UINT64,
                                                         G_TYPE_UINT,
                                                         G_TYPE_UINT,
                                                         G_TYPE_BOOLEAN,
                                                         G_TYPE_UINT,
                                                         G_TYPE_UINT,
                                                         G_TYPE_UINT,
                                                         G_TYPE_DOUBLE,
                                                         G_TYPE_DOUBLE);

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

    signals[SIGNAL_WINDOW_OBSERVATION_CONFIG] = g_signal_new("window-observation-config",
                                                             G_TYPE_FROM_CLASS(klass),
                                                             G_SIGNAL_RUN_LAST,
                                                             0,
                                                             NULL,
                                                             NULL,
                                                             NULL,
                                                             G_TYPE_NONE,
                                                             3,
                                                             G_TYPE_UINT64,
                                                             G_TYPE_STRV,
                                                             G_TYPE_STRV);

    signals[SIGNAL_WINDOW_OBSERVATION_RULES] = g_signal_new("window-observation-rules",
                                                            G_TYPE_FROM_CLASS(klass),
                                                            G_SIGNAL_RUN_LAST,
                                                            0,
                                                            NULL,
                                                            NULL,
                                                            NULL,
                                                            G_TYPE_NONE,
                                                            5,
                                                            G_TYPE_UINT64,
                                                            G_TYPE_STRV,
                                                            G_TYPE_STRV,
                                                            G_TYPE_STRV,
                                                            G_TYPE_STRV);

    signals[SIGNAL_PREFERRED_RENDER_DEVICE] = g_signal_new("preferred-render-device",
                                                           G_TYPE_FROM_CLASS(klass),
                                                           G_SIGNAL_RUN_LAST,
                                                           0,
                                                           NULL,
                                                           NULL,
                                                           NULL,
                                                           G_TYPE_NONE,
                                                           2,
                                                           G_TYPE_UINT,
                                                           G_TYPE_UINT);

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
    self->handle                   = waywallen_display_new(&cb);
    self->connected                = FALSE;
    self->shadow_fd                = -1;
    self->shadow_n_planes          = 0;
    self->shadow_modifier          = 0;
    self->shadow_buffer_generation = 0;
    self->shadow_valid             = FALSE;
    self->pause_effect_kind        = WW_PAUSE_EFFECT_KIND_NONE;
    self->blur_radius              = 30;
    self->transition_kind          = WW_TRANSITION_KIND_NONE;
    self->transition_duration_ms   = 400;
    self->transition_origin_x      = 0.5;
    self->transition_origin_y      = 0.5;
}

WwDisplay* ww_display_new(void) { return g_object_new(WW_TYPE_DISPLAY, NULL); }

void _ww_display_set_native_listener(WwDisplay* display, const WwDisplayNativeListener* listener,
                                     void* user_data) {
    g_return_if_fail(WW_IS_DISPLAY(display));
    g_return_if_fail(! display->native_listener_data || display->native_listener_data == user_data);
    display->native_listener      = listener ? *listener : (WwDisplayNativeListener) { 0 };
    display->native_listener_data = listener ? user_data : NULL;
}

void _ww_display_clear_native_listener(WwDisplay* display, void* user_data) {
    g_return_if_fail(WW_IS_DISPLAY(display));
    if (display->native_listener_data != user_data) return;
    display->native_listener      = (WwDisplayNativeListener) { 0 };
    display->native_listener_data = NULL;
}

gboolean ww_display_bind_dmabuf_relay(WwDisplay* self) {
    g_return_val_if_fail(WW_IS_DISPLAY(self), FALSE);
    g_return_val_if_fail(self->handle != NULL, FALSE);
    return waywallen_display_bind_dmabuf_relay(self->handle) == WAYWALLEN_OK;
}

gboolean ww_display_bind_dmabuf_relay_on_device(WwDisplay* self, guint major, guint minor) {
    g_return_val_if_fail(WW_IS_DISPLAY(self), FALSE);
    return waywallen_display_bind_dmabuf_relay_on_device(self->handle, major, minor) ==
           WAYWALLEN_OK;
}

gboolean ww_display_set_follow_producer_gpu(WwDisplay* self, gboolean enabled) {
    g_return_val_if_fail(WW_IS_DISPLAY(self), FALSE);
    return waywallen_display_set_render_device_callback(
               self->handle, enabled ? on_preferred_render_device : NULL, self) == WAYWALLEN_OK;
}

gboolean ww_display_set_presentation_capabilities(WwDisplay* self, guint flags) {
    g_return_val_if_fail(WW_IS_DISPLAY(self), FALSE);
    g_return_val_if_fail(self->handle != NULL, FALSE);
    return waywallen_display_set_presentation_caps(self->handle, flags) == WAYWALLEN_OK;
}

gboolean ww_display_set_pause_effect_capabilities(WwDisplay* self, guint flags) {
    g_return_val_if_fail(WW_IS_DISPLAY(self), FALSE);
    g_return_val_if_fail(self->handle != NULL, FALSE);
    return waywallen_display_set_pause_effect_caps(self->handle, flags) == WAYWALLEN_OK;
}

gboolean ww_display_set_transition_capabilities(WwDisplay* self, guint flags) {
    g_return_val_if_fail(WW_IS_DISPLAY(self), FALSE);
    g_return_val_if_fail(self->handle != NULL, FALSE);
    return waywallen_display_set_transition_caps(self->handle, flags) == WAYWALLEN_OK;
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

gboolean ww_display_set_window_observation_capabilities(WwDisplay* self, guint capabilities) {
    g_return_val_if_fail(WW_IS_DISPLAY(self), FALSE);
    return waywallen_display_set_window_observation_callback(
               self->handle,
               capabilities,
               capabilities ? on_window_observation_config : NULL,
               self) == WAYWALLEN_OK;
}

void ww_display_set_window_observation_state(WwDisplay* self, guint64 generation, guint flags) {
    g_return_if_fail(WW_IS_DISPLAY(self));
    (void)waywallen_display_set_window_observation_state(self->handle, generation, flags);
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
        self->transition_kind                = WW_TRANSITION_KIND_NONE;
        self->transition_duration_ms         = 400;
        self->transition_angle               = 0;
        self->transition_origin_x            = 0.5;
        self->transition_origin_y            = 0.5;
        notify_presentation(self);
        g_signal_emit(self,
                      signals[SIGNAL_PRESENTATION_SNAPSHOT],
                      0,
                      0,
                      0,
                      WW_PAUSE_EFFECT_KIND_NONE,
                      30u,
                      FALSE,
                      WW_TRANSITION_KIND_NONE,
                      400u,
                      0u,
                      0.5,
                      0.5);
    }
}
