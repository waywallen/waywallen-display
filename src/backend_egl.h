/*
 * Internal EGL backend for libwaywallen_display.
 *
 * Declares a ww_egl_backend_t function-pointer table and helpers for:
 *   - loading the table via dlopen/dlsym + eglGetProcAddress
 *   - importing DMA-BUF sets as `EGLImageKHR` via
 *     `EGL_EXT_image_dma_buf_import(_modifiers)`
 *   - creating GL_TEXTURE_EXTERNAL_OES textures from those EGLImages
 *   - waiting on acquire `dma_fence` sync_fds via
 *     `EGL_ANDROID_native_fence_sync`
 *
 * This header is compiled only when the build is configured with
 * `WAYWALLEN_DISPLAY_WITH_EGL=ON` (i.e. when `WW_HAVE_EGL` is
 * defined). The library never talks to libEGL/libGLESv2 unless that
 * define is set.
 *
 * All functions return 0 on success and a negative errno-style value
 * on failure. No function in this module ever calls `eglInitialize`,
 * creates a context, or assumes a current thread — that's the host
 * application's job.
 */

#ifndef WAYWALLEN_DISPLAY_BACKEND_EGL_H
#define WAYWALLEN_DISPLAY_BACKEND_EGL_H

#ifdef WW_HAVE_EGL

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  Function pointer table                                             */
/* ------------------------------------------------------------------ */

typedef struct ww_egl_backend {
    /* Core EGL (resolved from libEGL.so.1). */
    PFNEGLGETPROCADDRESSPROC eglGetProcAddress;
    EGLBoolean (*eglInitialize)(EGLDisplay, EGLint *, EGLint *);
    const char *(*eglQueryString)(EGLDisplay, EGLint);

    /* Extensions. Resolved via eglGetProcAddress. Any may be NULL if
     * the runtime driver lacks the corresponding extension, in which
     * case the loader returns -ENOSYS. */
    PFNEGLCREATEIMAGEKHRPROC            eglCreateImageKHR;
    PFNEGLDESTROYIMAGEKHRPROC           eglDestroyImageKHR;
    PFNEGLCREATESYNCKHRPROC             eglCreateSyncKHR;
    PFNEGLDESTROYSYNCKHRPROC            eglDestroySyncKHR;
    PFNEGLWAITSYNCKHRPROC               eglWaitSyncKHR;
    PFNEGLCLIENTWAITSYNCKHRPROC         eglClientWaitSyncKHR;
    PFNEGLDUPNATIVEFENCEFDANDROIDPROC   eglDupNativeFenceFDANDROID;

    /* GL_OES_EGL_image entry point. */
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES;

    /* EGL_EXT_image_dma_buf_import_modifiers cap probe. Optional —
     * older Mesa lacks the modifier-aware extension, in which case
     * `ww_egl_query_format_caps` returns -ENOSYS and the caller
     * falls back to a hardcoded LINEAR-only set. */
    PFNEGLQUERYDMABUFFORMATSEXTPROC   eglQueryDmaBufFormatsEXT;
    PFNEGLQUERYDMABUFMODIFIERSEXTPROC eglQueryDmaBufModifiersEXT;

    /* Core GLES2 (resolved from libGLESv2.so.2). */
    void (*glGenTextures)(GLsizei, GLuint *);
    void (*glDeleteTextures)(GLsizei, const GLuint *);
    void (*glBindTexture)(GLenum, GLuint);
    void (*glTexParameteri)(GLenum, GLenum, GLint);

    /* True once all required symbols above are resolved. */
    bool loaded;
} ww_egl_backend_t;

/* Host-provided loader hint. May be NULL, in which case the backend
 * falls back to `dlopen("libEGL.so.1") + dlsym` and
 * `eglGetProcAddress`. When non-NULL it is called first for every
 * symbol (both core and extension) and only falls back if it returns
 * NULL. */
typedef void *(*ww_egl_get_proc_address_fn)(const char *name);

/* Populate the function-pointer table. Returns 0 on success, -errno
 * on failure (typically -ENOSYS if a required extension symbol is
 * missing, or -ENOENT if libEGL/libGLESv2 can't be dlopened). */
int ww_egl_backend_load(ww_egl_backend_t *backend,
                        ww_egl_get_proc_address_fn host_get_proc_address);

/* Release dlopen handles the backend may have opened. Safe to call
 * on a zero-initialised backend. */
void ww_egl_backend_unload(ww_egl_backend_t *backend);

/* ------------------------------------------------------------------ */
/*  DMA-BUF import                                                     */
/* ------------------------------------------------------------------ */

#define WW_EGL_MAX_PLANES 4

typedef struct ww_egl_dmabuf_import {
    EGLDisplay egl_display;
    uint32_t fourcc;
    uint32_t width;
    uint32_t height;
    uint64_t modifier;         /* DRM_FORMAT_MOD_*; use LINEAR for untiled */
    uint32_t n_planes;         /* 1..WW_EGL_MAX_PLANES */
    int fds[WW_EGL_MAX_PLANES];
    uint32_t strides[WW_EGL_MAX_PLANES];
    uint32_t offsets[WW_EGL_MAX_PLANES];
} ww_egl_dmabuf_import_t;

/* Build an EGLImageKHR from a DMA-BUF set. On success `*out_image`
 * is populated and owned by the caller, who must later release it
 * via `ww_egl_destroy_image`. The input file descriptors are NOT
 * closed by this function — the EGL driver dup2's them internally. */
int ww_egl_import_dmabuf(const ww_egl_backend_t *backend,
                         const ww_egl_dmabuf_import_t *import,
                         EGLImageKHR *out_image);

/* Destroy an EGLImage returned by ww_egl_import_dmabuf. */
void ww_egl_destroy_image(const ww_egl_backend_t *backend,
                          EGLDisplay display,
                          EGLImageKHR image);

/* Create a fresh GL_TEXTURE_EXTERNAL_OES texture, bind the EGLImage
 * to it, and return its name. Caller must have a GL context current
 * on the calling thread. */
int ww_egl_texture_from_image(const ww_egl_backend_t *backend,
                              EGLImageKHR image,
                              GLuint *out_tex);

/* ------------------------------------------------------------------ */
/*  DRM render-node introspection                                      */
/* ------------------------------------------------------------------ */

/*
 * Look up the DRM render-node major/minor of the GPU backing
 * `egl_display`. Uses `EGL_EXT_device_query` to map the EGLDisplay to
 * an `EGLDeviceEXT`, then `EGL_DRM_DEVICE_FILE_EXT` to a path under
 * `/dev/dri/` whose `st_rdev` carries the (major, minor) we report on
 * the wire as `register_display.drm_render_*`.
 *
 * Returns 0 on success and writes the values into `*out_major` /
 * `*out_minor`. Returns -ENOSYS if the runtime EGL driver doesn't
 * advertise the necessary extensions, or any other negative errno on
 * lookup failure. Callers should treat any non-zero return as
 * "unknown" and report `(0, 0)` so the daemon falls back to the
 * cross-GPU host-visible path.
 */
int ww_egl_query_drm_render_node(const ww_egl_backend_t *backend,
                                 EGLDisplay egl_display,
                                 uint32_t *out_major,
                                 uint32_t *out_minor);

/* ------------------------------------------------------------------ */
/*  Format/modifier capability probe                                   */
/* ------------------------------------------------------------------ */

/*
 * Streaming emit callback used by `ww_egl_query_format_caps` to feed
 * each accepted (fourcc, modifier) pair to the caller. `plane_count`
 * mirrors the wire field in `consumer_caps`.
 */
typedef void (*ww_egl_caps_emit_fn)(uint32_t fourcc,
                                    uint64_t modifier,
                                    uint32_t plane_count,
                                    void *user_data);

/*
 * Enumerate the (fourcc, modifier) set the driver advertises as
 * importable via `EGL_EXT_image_dma_buf_import_modifiers`. For each
 * accepted pair (external_only=false) the `emit` callback is invoked
 * once. Caller is responsible for filtering / deduplication.
 *
 * Returns 0 on success, -ENOSYS when the EXT isn't available, or
 * -errno from a failed query. On any error `emit` is not invoked at
 * all, so the caller can safely fall back to a hardcoded cap set.
 */
int ww_egl_query_format_caps(const ww_egl_backend_t *backend,
                             EGLDisplay egl_display,
                             ww_egl_caps_emit_fn emit,
                             void *user_data);

/* ------------------------------------------------------------------ */
/*  Acquire sync fence                                                 */
/* ------------------------------------------------------------------ */

/*
 * Consume a `dma_fence` sync_fd and queue a GPU-side wait on the
 * current GL context. On success the fd ownership transfers to EGL
 * (the kernel closes it when the wrapping EGLSync is destroyed). On
 * failure the caller retains the fd and must close it itself.
 *
 * Requires the driver to advertise `EGL_ANDROID_native_fence_sync`.
 */
int ww_egl_wait_sync_fd(const ww_egl_backend_t *backend,
                        EGLDisplay display,
                        int sync_fd);

#ifdef __cplusplus
}
#endif

#endif /* WW_HAVE_EGL */

#endif /* WAYWALLEN_DISPLAY_BACKEND_EGL_H */
