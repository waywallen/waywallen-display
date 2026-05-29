#define _GNU_SOURCE
#include "ww-shadow-paintable.h"

#include <gtk/gtk.h>
#include <graphene.h>

#include <errno.h>

#include <fcntl.h>
#include <math.h>
#include <string.h>
#include <unistd.h>

struct _WwShadowPaintable {
    GObject     parent_instance;

    GdkTexture *tex;          /* current generation, owned ref */
    GdkDisplay *display;      /* borrowed */

    gint     fd;              /* owned shadow DMA-BUF fd, -1 if cleared */
    guint    n_planes;
    guint    width;
    guint    height;
    guint    fourcc;
    guint64  modifier;
    guint    strides[4];
    guint64  offsets[4];
    gboolean have_shadow;

    /* Layout from WwDisplay::config (set_config). have_config false until
     * the first config arrives — then snapshot stretches full shadow. */
    gboolean have_config;
    float    src[4];          /* x, y, w, h in texture px */
    float    dst[4];          /* x, y, w, h in pre-rotation display px */
    guint    transform;       /* wl_output.transform 0-7 */
    float    clear[4];        /* letterbox RGBA */
};

static void ww_shadow_paintable_iface_init(GdkPaintableInterface *iface);

G_DEFINE_TYPE_WITH_CODE(WwShadowPaintable, ww_shadow_paintable, G_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE(GDK_TYPE_PAINTABLE, ww_shadow_paintable_iface_init))

/* Per-texture dup'd fds. GdkDmabufTextureBuilder does NOT close the
 * fds it's handed (gdk/gdkdmabuftexturebuilder.c:1046) — the caller
 * owns them for the texture's lifetime. We pass close_dup_fds as the
 * build destroy notify so they're closed exactly when the texture
 * finalizes. Without this each frame leaks n_planes fds. */
typedef struct {
    int fds[4];
    guint n;
} WwDupFds;

static void
close_dup_fds(gpointer data)
{
    WwDupFds *d = data;
    for (guint i = 0; i < d->n; i++) {
        if (d->fds[i] >= 0)
            close(d->fds[i]);
    }
    g_free(d);
}

static GdkTexture *
build_texture(WwShadowPaintable *self)
{
    if (self->fd < 0)
        return NULL;
    if (!self->display)
        self->display = gdk_display_get_default();

    WwDupFds *dups = g_new0(WwDupFds, 1);
    dups->n = self->n_planes;

    GdkDmabufTextureBuilder *b = gdk_dmabuf_texture_builder_new();
    gdk_dmabuf_texture_builder_set_display(b, self->display);
    gdk_dmabuf_texture_builder_set_width(b, self->width);
    gdk_dmabuf_texture_builder_set_height(b, self->height);
    gdk_dmabuf_texture_builder_set_fourcc(b, self->fourcc);
    gdk_dmabuf_texture_builder_set_modifier(b, self->modifier);
    gdk_dmabuf_texture_builder_set_n_planes(b, self->n_planes);
    for (guint p = 0; p < self->n_planes; p++) {
        int dup_fd = fcntl(self->fd, F_DUPFD_CLOEXEC, 0);
        if (dup_fd < 0) {
            g_warning("ww_shadow_paintable: dup fd failed: %s",
                      g_strerror(errno));
            close_dup_fds(dups);
            g_object_unref(b);
            return NULL;
        }
        dups->fds[p] = dup_fd;
        gdk_dmabuf_texture_builder_set_fd(b, p, dup_fd);
        gdk_dmabuf_texture_builder_set_stride(b, p, self->strides[p]);
        gdk_dmabuf_texture_builder_set_offset(b, p, self->offsets[p]);
    }

    GError *err = NULL;
    GdkTexture *tex = gdk_dmabuf_texture_builder_build(b, close_dup_fds,
                                                       dups, &err);
    if (!tex) {
        g_warning("ww_shadow_paintable: dmabuf build failed: %s",
                  err ? err->message : "?");
        g_clear_error(&err);
        close_dup_fds(dups);
    }
    g_object_unref(b);
    return tex;
}

/* Draw source rect of self->tex into dest rect: scale the full texture
 * so src maps onto dst, clip to dst. */
static void
draw_src_to_dst(WwShadowPaintable *self, GtkSnapshot *s,
                const float src[4], const float dst[4])
{
    float sw = src[2] > 0 ? src[2] : (float)self->width;
    float sh = src[3] > 0 ? src[3] : (float)self->height;
    float kx = dst[2] / sw;
    float ky = dst[3] / sh;

    graphene_rect_t clip;
    graphene_rect_init(&clip, dst[0], dst[1], dst[2], dst[3]);
    gtk_snapshot_push_clip(s, &clip);
    gtk_snapshot_save(s);

    graphene_point_t off;
    graphene_point_init(&off, dst[0] - src[0] * kx, dst[1] - src[1] * ky);
    gtk_snapshot_translate(s, &off);
    gtk_snapshot_scale(s, kx, ky);

    graphene_rect_t full;
    graphene_rect_init(&full, 0.0f, 0.0f,
                       (float)self->width, (float)self->height);
    gtk_snapshot_append_texture(s, self->tex, &full);

    gtk_snapshot_restore(s);
    gtk_snapshot_pop(s);  /* clip */
}

static void
snapshot_vfunc(GdkPaintable *paintable,
               GdkSnapshot  *snapshot,
               double        width,
               double        height)
{
    WwShadowPaintable *self = WW_SHADOW_PAINTABLE(paintable);
    GtkSnapshot *s = GTK_SNAPSHOT(snapshot);
    if (!self->tex)
        return;
    if (!isfinite(width) || width <= 0.0)
        width = (double)self->width;
    if (!isfinite(height) || height <= 0.0)
        height = (double)self->height;

    if (!self->have_config) {
        graphene_rect_t rect;
        graphene_rect_init(&rect, 0.0f, 0.0f, (float)width, (float)height);
        gtk_snapshot_append_texture(s, self->tex, &rect);
        return;
    }

    /* dst is in pre-rotation display space; for 90/270 that space has
     * swapped W/H. Rotate the canvas about the widget centre so the
     * pre-rotation drawing lands on the physical display, then draw
     * src→dst there. */
    int t = (int)self->transform;
    gboolean swap = (t == 1 || t == 3 || t == 5 || t == 7);
    float preW = swap ? (float)height : (float)width;
    float preH = swap ? (float)width  : (float)height;

    gtk_snapshot_save(s);

    graphene_point_t c;
    graphene_point_init(&c, (float)width / 2.0f, (float)height / 2.0f);
    gtk_snapshot_translate(s, &c);
    if (t >= 4) {  /* flipped variants: mirror X before rotating */
        gtk_snapshot_scale(s, -1.0f, 1.0f);
        gtk_snapshot_rotate(s, (float)((t - 4) * 90));
    } else if (t != 0) {
        gtk_snapshot_rotate(s, (float)(t * 90));
    }
    graphene_point_init(&c, -preW / 2.0f, -preH / 2.0f);
    gtk_snapshot_translate(s, &c);

    if (self->clear[3] > 0.0f) {
        GdkRGBA bg = { self->clear[0], self->clear[1],
                       self->clear[2], self->clear[3] };
        graphene_rect_t full;
        graphene_rect_init(&full, 0.0f, 0.0f, preW, preH);
        gtk_snapshot_append_color(s, &bg, &full);
    }
    draw_src_to_dst(self, s, self->src, self->dst);

    gtk_snapshot_restore(s);
}

static int
intrinsic_width_vfunc(GdkPaintable *paintable)
{
    return (int)WW_SHADOW_PAINTABLE(paintable)->width;
}

static int
intrinsic_height_vfunc(GdkPaintable *paintable)
{
    return (int)WW_SHADOW_PAINTABLE(paintable)->height;
}

static double
intrinsic_aspect_vfunc(GdkPaintable *paintable)
{
    WwShadowPaintable *self = WW_SHADOW_PAINTABLE(paintable);
    return self->height > 0
        ? (double)self->width / (double)self->height
        : 0.0;
}

static void
ww_shadow_paintable_iface_init(GdkPaintableInterface *iface)
{
    iface->snapshot = snapshot_vfunc;
    iface->get_intrinsic_width = intrinsic_width_vfunc;
    iface->get_intrinsic_height = intrinsic_height_vfunc;
    iface->get_intrinsic_aspect_ratio = intrinsic_aspect_vfunc;
}

static void
ww_shadow_paintable_init(WwShadowPaintable *self)
{
    self->fd = -1;
}

static void
ww_shadow_paintable_finalize(GObject *object)
{
    WwShadowPaintable *self = WW_SHADOW_PAINTABLE(object);
    g_clear_object(&self->tex);
    if (self->fd >= 0) {
        close(self->fd);
        self->fd = -1;
    }
    G_OBJECT_CLASS(ww_shadow_paintable_parent_class)->finalize(object);
}

static void
ww_shadow_paintable_class_init(WwShadowPaintableClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = ww_shadow_paintable_finalize;
}

WwShadowPaintable *
ww_shadow_paintable_new(void)
{
    return g_object_new(WW_TYPE_SHADOW_PAINTABLE, NULL);
}

gboolean
ww_shadow_paintable_set_shadow(WwShadowPaintable *self,
                               gint               fd,
                               guint              n_planes,
                               guint              width,
                               guint              height,
                               guint              fourcc,
                               guint64            modifier,
                               const guint       *strides,
                               const guint64     *offsets)
{
    g_return_val_if_fail(WW_IS_SHADOW_PAINTABLE(self), FALSE);
    g_return_val_if_fail(fd >= 0, FALSE);
    g_return_val_if_fail(n_planes > 0 && n_planes <= 4, FALSE);
    g_return_val_if_fail(strides && offsets, FALSE);

    if (self->fd >= 0)
        close(self->fd);
    self->fd       = fd;
    self->n_planes = n_planes;
    self->width    = width;
    self->height   = height;
    self->fourcc   = fourcc;
    self->modifier = modifier;
    memcpy(self->strides, strides, sizeof(guint)   * n_planes);
    memcpy(self->offsets, offsets, sizeof(guint64) * n_planes);
    for (guint p = n_planes; p < 4; p++) {
        self->strides[p] = 0;
        self->offsets[p] = 0;
    }
    self->have_shadow = TRUE;

    GdkTexture *tex = build_texture(self);
    g_clear_object(&self->tex);
    self->tex = tex;

    gdk_paintable_invalidate_size(GDK_PAINTABLE(self));
    gdk_paintable_invalidate_contents(GDK_PAINTABLE(self));
    return tex != NULL;
}

void
ww_shadow_paintable_refresh(WwShadowPaintable *self)
{
    g_return_if_fail(WW_IS_SHADOW_PAINTABLE(self));
    if (!self->have_shadow)
        return;
    /* Rebuild GdkDmabufTexture every frame. GTK's gskvulkanimage.c only
     * reads dma_resv into a TEMPORARY semaphore at import time, then
     * consumes it on the first layout transition — subsequent samples
     * of a cached VkImage do not re-sync. So the only way fresh shadow
     * content reaches the screen is to force GSK to re-import, which
     * requires a new GdkTexture pointer (gsk_gpu_cache_lookup_texture_image
     * keys on it). Blitter has injected the write fence into shadow's
     * dma_resv right before we get here, so the fresh import's
     * EXPORT_SYNC_FILE captures it and GSK waits on first sample.
     * g_clear_object is synchronous: when we hold the last ref, the
     * old GdkTexture finalizes immediately, fires gsk_gpu_cache's
     * weak_ref destroy_cb, and bumps dead_textures counter so the
     * pre-frame GC evicts the cached VkImage. */
    GdkTexture *tex = build_texture(self);
    if (!tex)
        return;
    g_clear_object(&self->tex);
    self->tex = tex;
    gdk_paintable_invalidate_contents(GDK_PAINTABLE(self));
}

void
ww_shadow_paintable_set_config(WwShadowPaintable *self,
                               double sx, double sy, double sw, double sh,
                               double dx, double dy, double dw, double dh,
                               guint transform,
                               double cr, double cg, double cb, double ca)
{
    g_return_if_fail(WW_IS_SHADOW_PAINTABLE(self));
    self->src[0] = (float)sx; self->src[1] = (float)sy;
    self->src[2] = (float)sw; self->src[3] = (float)sh;
    self->dst[0] = (float)dx; self->dst[1] = (float)dy;
    self->dst[2] = (float)dw; self->dst[3] = (float)dh;
    self->transform = transform;
    self->clear[0] = (float)cr; self->clear[1] = (float)cg;
    self->clear[2] = (float)cb; self->clear[3] = (float)ca;
    self->have_config = TRUE;
    gdk_paintable_invalidate_contents(GDK_PAINTABLE(self));
}

void
ww_shadow_paintable_clear(WwShadowPaintable *self)
{
    g_return_if_fail(WW_IS_SHADOW_PAINTABLE(self));
    g_clear_object(&self->tex);
    if (self->fd >= 0) {
        close(self->fd);
        self->fd = -1;
    }
    self->have_shadow = FALSE;
    gdk_paintable_invalidate_contents(GDK_PAINTABLE(self));
}
