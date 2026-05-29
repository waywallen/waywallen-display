/*
 * WwShadowPaintable — GdkPaintable backed by a single shadow DMA-BUF
 * whose content is mutated externally by an out-of-process producer.
 *
 * GSK keys its imported-VkImage cache by GdkTexture pointer and evicts
 * via the texture's weak_ref. To make on-screen content track external
 * writes, refresh() rebuilds the underlying GdkDmabufTexture and
 * g_object_unrefs the previous one — synchronously, so the prior
 * VkImage is released immediately and the cache stays bounded. Doing
 * this from JS leaks VkImages because GJS GC is lazy.
 */

#ifndef WW_SHADOW_PAINTABLE_H
#define WW_SHADOW_PAINTABLE_H

#include <glib-object.h>

G_BEGIN_DECLS

#define WW_TYPE_SHADOW_PAINTABLE (ww_shadow_paintable_get_type())
G_DECLARE_FINAL_TYPE(WwShadowPaintable, ww_shadow_paintable,
                     WW, SHADOW_PAINTABLE, GObject)

/**
 * ww_shadow_paintable_new:
 *
 * Returns: (transfer full): a fresh paintable with no shadow attached.
 *   Pass to e.g. gtk_picture_set_paintable() immediately; call
 *   ww_shadow_paintable_set_shadow() once the producer publishes a
 *   shadow descriptor, then ww_shadow_paintable_refresh() per frame.
 */
WwShadowPaintable *ww_shadow_paintable_new(void);

/**
 * ww_shadow_paintable_set_shadow:
 * @self: a #WwShadowPaintable
 * @fd: shadow DMA-BUF fd — paintable takes ownership and closes on
 *   clear/finalize. Pass a fresh dup'd fd; do NOT close it after this
 *   call.
 * @n_planes: number of planes (1..4)
 * @width: image width in pixels
 * @height: image height in pixels
 * @fourcc: DRM fourcc
 * @modifier: DRM format modifier
 * @strides: (array fixed-size=4): per-plane byte pitch
 * @offsets: (array fixed-size=4): per-plane byte offset
 *
 * Bind the paintable to a new shadow generation. Builds the first
 * GdkDmabufTexture and emits invalidate-size + invalidate-contents.
 *
 * Returns: TRUE on success
 */
gboolean ww_shadow_paintable_set_shadow(WwShadowPaintable *self,
                                        gint fd,
                                        guint n_planes,
                                        guint width,
                                        guint height,
                                        guint fourcc,
                                        guint64 modifier,
                                        const guint *strides,
                                        const guint64 *offsets);

/**
 * ww_shadow_paintable_refresh:
 * @self: a #WwShadowPaintable
 *
 * Rebuild the GdkDmabufTexture for the current shadow and drop the
 * previous one synchronously. Picture re-snapshots on the
 * invalidate-contents signal and GSK re-imports the DMA-BUF, so screen
 * content tracks producer writes. Cheap (FD dup + ~512 B object); call
 * once per producer frame_ready.
 */
void ww_shadow_paintable_refresh(WwShadowPaintable *self);

/**
 * ww_shadow_paintable_set_config:
 * @self: a #WwShadowPaintable
 * @sx: @sy: @sw: @sh: source rect in texture pixels (sub-region to sample)
 * @dx: @dy: @dw: @dh: dest rect in pre-rotation display pixels
 * @transform: wl_output.transform (0=normal, 1/2/3 = 90/180/270 CW,
 *   4-7 = flipped variants) applied after placing source→dest
 * @cr: @cg: @cb: @ca: letterbox clear color (straight-alpha sRGB)
 *
 * Fill mode / align / rotation, projected daemon-side and delivered via
 * the WwDisplay::config signal. Until this is called the paintable
 * stretches the whole shadow over the widget.
 */
void ww_shadow_paintable_set_config(WwShadowPaintable *self,
                                    double sx, double sy, double sw, double sh,
                                    double dx, double dy, double dw, double dh,
                                    guint transform,
                                    double cr, double cg, double cb, double ca);

/**
 * ww_shadow_paintable_clear:
 * @self: a #WwShadowPaintable
 *
 * Release the texture + shadow fd; paintable renders empty until the
 * next set_shadow().
 */
void ww_shadow_paintable_clear(WwShadowPaintable *self);

G_END_DECLS

#endif /* WW_SHADOW_PAINTABLE_H */
