/*
 * libwaywallen_display — EGL backend.
 *
 * Compiled only when WW_HAVE_EGL is defined (see CMakeLists.txt).
 *
 * This file implements the function-pointer loader and the three
 * operations the dispatch path actually needs:
 *
 *   1. DMA-BUF → EGLImageKHR  (ww_egl_import_dmabuf)
 *   2. EGLImageKHR → GL_TEXTURE_2D  (ww_egl_texture_from_image)
 *   3. dma_fence sync_fd → EGLSyncKHR wait  (ww_egl_wait_sync_fd)
 *
 * All operations are compile-only validated in this phase — the
 * developer box has libEGL/libGLESv2 but no display to actually
 * exercise the import path. Runtime validation arrives in Phase 3b
 * alongside producer-side sync_fd export.
 */

#ifdef WW_HAVE_EGL

#define _GNU_SOURCE

#include "backend_egl.h"
#include "log_internal.h"

#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * DRM_FORMAT_MOD_INVALID lives in <drm/drm_fourcc.h> on most distros
 * but the kernel uapi header isn't always in the user include path
 * (e.g. Fedora ships it only with kernel-headers-devel). We define
 * the well-known value here as a fallback so the EGL backend doesn't
 * gain a kernel-headers build dependency. Matches Mesa / Wayland
 * conventions.
 */
#ifndef DRM_FORMAT_MOD_INVALID
#define DRM_FORMAT_MOD_INVALID ((1ULL << 56) - 1ULL)
#endif

/* ------------------------------------------------------------------ */
/*  Loader                                                             */
/* ------------------------------------------------------------------ */

/*
 * Track the dlopen handles so we can dlclose them on unload. These
 * are process-global state guarded implicitly by the single-threaded
 * backend contract (the host owns the GL thread).
 */
static void *s_libegl = NULL;
static void *s_libgles = NULL;

static void *load_from_dl(const char *libname, void **handle_out) {
    if (*handle_out) return *handle_out;
    void *h = dlopen(libname, RTLD_LAZY | RTLD_LOCAL);
    if (h) *handle_out = h;
    return h;
}

/*
 * POSIX.1 rationale §dlsym: object/function pointer conversion goes
 * through a union to keep -Wpedantic happy. The returned value is
 * opaque — the caller knows the expected signature and casts through
 * its own (TYPE) cast.
 */
typedef union ww_ptr_cvt {
    void *obj;
    void (*func)(void);
} ww_ptr_cvt_t;

static void (*resolve_symbol(ww_egl_get_proc_address_fn host_getproc,
                             PFNEGLGETPROCADDRESSPROC egl_getproc,
                             void *dl_fallback,
                             const char *name))(void) {
    if (host_getproc) {
        void *p = host_getproc(name);
        if (p) {
            ww_ptr_cvt_t c;
            c.obj = p;
            return c.func;
        }
    }
    if (egl_getproc) {
        /* eglGetProcAddress returns a function pointer directly. */
        __eglMustCastToProperFunctionPointerType f = egl_getproc(name);
        if (f) return (void (*)(void))f;
    }
    if (dl_fallback) {
        void *p = dlsym(dl_fallback, name);
        if (p) {
            ww_ptr_cvt_t c;
            c.obj = p;
            return c.func;
        }
    }
    return NULL;
}

int ww_egl_backend_load(ww_egl_backend_t *backend,
                        ww_egl_get_proc_address_fn host_get_proc_address) {
    if (!backend) return -EINVAL;
    memset(backend, 0, sizeof(*backend));

    /* Step 1: resolve eglGetProcAddress. Priority:
     *   host-provided loader → dlsym(libEGL.so.1) → fail. */
    PFNEGLGETPROCADDRESSPROC egl_getproc = NULL;
    if (host_get_proc_address) {
        ww_ptr_cvt_t c;
        c.obj = host_get_proc_address("eglGetProcAddress");
        if (c.obj) egl_getproc = (PFNEGLGETPROCADDRESSPROC)c.func;
    }
    if (!egl_getproc) {
        if (!load_from_dl("libEGL.so.1", &s_libegl)) {
            ww_log(WAYWALLEN_LOG_WARN,
                   "egl: dlopen(libEGL.so.1) failed: %s", dlerror());
            return -ENOENT;
        }
        ww_ptr_cvt_t c;
        c.obj = dlsym(s_libegl, "eglGetProcAddress");
        if (c.obj) egl_getproc = (PFNEGLGETPROCADDRESSPROC)c.func;
    }
    if (!egl_getproc) {
        ww_log(WAYWALLEN_LOG_WARN, "egl: cannot resolve eglGetProcAddress");
        return -ENOSYS;
    }
    backend->eglGetProcAddress = egl_getproc;

    /* Step 2: open libGLESv2.so.2 as a symbol source for core GLES
     * entries. Most drivers also expose these through
     * eglGetProcAddress, but dlsym is the canonical fallback. */
    if (!s_libgles) {
        (void)load_from_dl("libGLESv2.so.2", &s_libgles);
    }

/* Convenience macro: resolve `name` via host / eglGetProcAddress /
 * dlsym(libhandle). Assign to the matching function-pointer slot in
 * `backend`. On NULL, log the missing symbol and return -ENOSYS. */
#define RESOLVE_REQUIRED(SLOT, TYPE, NAME, DL) do {                 \
        void (*f)(void) = resolve_symbol(host_get_proc_address,     \
                                         backend->eglGetProcAddress,\
                                         (DL), NAME);               \
        if (!f) {                                                   \
            ww_log(WAYWALLEN_LOG_WARN,                              \
                   "egl: cannot resolve symbol %s", NAME);          \
            return -ENOSYS;                                         \
        }                                                           \
        backend->SLOT = (TYPE)f;                                    \
    } while (0)

    /* Core EGL. */
    RESOLVE_REQUIRED(eglInitialize,
                     EGLBoolean (*)(EGLDisplay, EGLint *, EGLint *),
                     "eglInitialize", s_libegl);
    RESOLVE_REQUIRED(eglQueryString,
                     const char *(*)(EGLDisplay, EGLint),
                     "eglQueryString", s_libegl);

    /* Extensions (KHR/EXT/ANDROID). These live in the extension
     * surface so eglGetProcAddress is the canonical lookup. */
    RESOLVE_REQUIRED(eglCreateImageKHR,
                     PFNEGLCREATEIMAGEKHRPROC,
                     "eglCreateImageKHR", s_libegl);
    RESOLVE_REQUIRED(eglDestroyImageKHR,
                     PFNEGLDESTROYIMAGEKHRPROC,
                     "eglDestroyImageKHR", s_libegl);
    RESOLVE_REQUIRED(eglCreateSyncKHR,
                     PFNEGLCREATESYNCKHRPROC,
                     "eglCreateSyncKHR", s_libegl);
    RESOLVE_REQUIRED(eglDestroySyncKHR,
                     PFNEGLDESTROYSYNCKHRPROC,
                     "eglDestroySyncKHR", s_libegl);
    RESOLVE_REQUIRED(eglWaitSyncKHR,
                     PFNEGLWAITSYNCKHRPROC,
                     "eglWaitSyncKHR", s_libegl);
    RESOLVE_REQUIRED(eglClientWaitSyncKHR,
                     PFNEGLCLIENTWAITSYNCKHRPROC,
                     "eglClientWaitSyncKHR", s_libegl);
    RESOLVE_REQUIRED(eglDupNativeFenceFDANDROID,
                     PFNEGLDUPNATIVEFENCEFDANDROIDPROC,
                     "eglDupNativeFenceFDANDROID", s_libegl);

    /* GL_OES_EGL_image: glEGLImageTargetTexture2DOES lives in libGLESv2
     * but is typically resolved through eglGetProcAddress so drivers
     * can substitute per-context implementations. */
    RESOLVE_REQUIRED(glEGLImageTargetTexture2DOES,
                     PFNGLEGLIMAGETARGETTEXTURE2DOESPROC,
                     "glEGLImageTargetTexture2DOES", s_libgles);

    /* Core GLES2. Present in libGLESv2.so.2. */
    RESOLVE_REQUIRED(glGenTextures,
                     void (*)(GLsizei, GLuint *),
                     "glGenTextures", s_libgles);
    RESOLVE_REQUIRED(glDeleteTextures,
                     void (*)(GLsizei, const GLuint *),
                     "glDeleteTextures", s_libgles);
    RESOLVE_REQUIRED(glBindTexture,
                     void (*)(GLenum, GLuint),
                     "glBindTexture", s_libgles);
    RESOLVE_REQUIRED(glTexParameteri,
                     void (*)(GLenum, GLenum, GLint),
                     "glTexParameteri", s_libgles);

#undef RESOLVE_REQUIRED

/* Optional symbols: missing → NULL slot, no error. The cap probe
 * checks for non-NULL before invoking; callers fall back to a
 * hardcoded LINEAR-only set when the driver lacks the EXT. */
#define RESOLVE_OPTIONAL(SLOT, TYPE, NAME, DL) do {                 \
        void (*f)(void) = resolve_symbol(host_get_proc_address,     \
                                         backend->eglGetProcAddress,\
                                         (DL), NAME);               \
        backend->SLOT = (TYPE)f;                                    \
    } while (0)

    RESOLVE_OPTIONAL(eglQueryDmaBufFormatsEXT,
                     PFNEGLQUERYDMABUFFORMATSEXTPROC,
                     "eglQueryDmaBufFormatsEXT", s_libegl);
    RESOLVE_OPTIONAL(eglQueryDmaBufModifiersEXT,
                     PFNEGLQUERYDMABUFMODIFIERSEXTPROC,
                     "eglQueryDmaBufModifiersEXT", s_libegl);

#undef RESOLVE_OPTIONAL

    backend->loaded = true;
    return 0;
}

void ww_egl_backend_unload(ww_egl_backend_t *backend) {
    if (backend) {
        memset(backend, 0, sizeof(*backend));
    }
    /* Leave the dlopen handles alive: they're process-global and
     * re-loading is idempotent. dlclose here would race any other
     * component in the same process that holds an fn pointer.
     *
     * Tests that want a fresh state should spawn a subprocess. */
}

/* ------------------------------------------------------------------ */
/*  DMA-BUF import                                                     */
/* ------------------------------------------------------------------ */

/* Per-plane attribute triplet tables, indexed by plane number.
 * EGL_DMA_BUF_PLANEn_FD_EXT etc. are each consecutive enum values
 * per plane, but we just encode them explicitly to keep the code
 * legible. */
static const EGLint k_plane_attr_fd[WW_EGL_MAX_PLANES] = {
    EGL_DMA_BUF_PLANE0_FD_EXT,
    EGL_DMA_BUF_PLANE1_FD_EXT,
    EGL_DMA_BUF_PLANE2_FD_EXT,
    EGL_DMA_BUF_PLANE3_FD_EXT,
};
static const EGLint k_plane_attr_offset[WW_EGL_MAX_PLANES] = {
    EGL_DMA_BUF_PLANE0_OFFSET_EXT,
    EGL_DMA_BUF_PLANE1_OFFSET_EXT,
    EGL_DMA_BUF_PLANE2_OFFSET_EXT,
    EGL_DMA_BUF_PLANE3_OFFSET_EXT,
};
static const EGLint k_plane_attr_pitch[WW_EGL_MAX_PLANES] = {
    EGL_DMA_BUF_PLANE0_PITCH_EXT,
    EGL_DMA_BUF_PLANE1_PITCH_EXT,
    EGL_DMA_BUF_PLANE2_PITCH_EXT,
    EGL_DMA_BUF_PLANE3_PITCH_EXT,
};
static const EGLint k_plane_attr_mod_lo[WW_EGL_MAX_PLANES] = {
    EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
    EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
    EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT,
    EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT,
};
static const EGLint k_plane_attr_mod_hi[WW_EGL_MAX_PLANES] = {
    EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
    EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT,
    EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT,
    EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT,
};

int ww_egl_import_dmabuf(const ww_egl_backend_t *backend,
                         const ww_egl_dmabuf_import_t *im,
                         EGLImageKHR *out_image) {
    if (!backend || !im || !out_image) return -EINVAL;
    if (!backend->loaded) return -ENOSYS;
    if (im->n_planes == 0 || im->n_planes > WW_EGL_MAX_PLANES) return -EINVAL;

    /* Attribute list layout:
     *   width, height, linux_drm_fourcc_ext,
     *   PLANEn_FD, PLANEn_OFFSET, PLANEn_PITCH,
     *   [PLANEn_MODIFIER_LO, PLANEn_MODIFIER_HI]  (if modifier != INVALID)
     *   EGL_NONE
     *
     * Max attributes:
     *   3 header pairs (6 entries) + 5 pairs per plane (10 entries) * 4
     *   = 6 + 40 = 46 entries + 1 terminator = 47.
     */
    EGLint attrs[48];
    size_t i = 0;
    attrs[i++] = EGL_WIDTH;
    attrs[i++] = (EGLint)im->width;
    attrs[i++] = EGL_HEIGHT;
    attrs[i++] = (EGLint)im->height;
    attrs[i++] = EGL_LINUX_DRM_FOURCC_EXT;
    attrs[i++] = (EGLint)im->fourcc;

    const bool emit_modifier = (im->modifier != DRM_FORMAT_MOD_INVALID);
    for (uint32_t p = 0; p < im->n_planes; p++) {
        attrs[i++] = k_plane_attr_fd[p];
        attrs[i++] = (EGLint)im->fds[p];
        attrs[i++] = k_plane_attr_offset[p];
        attrs[i++] = (EGLint)im->offsets[p];
        attrs[i++] = k_plane_attr_pitch[p];
        attrs[i++] = (EGLint)im->strides[p];
        if (emit_modifier) {
            attrs[i++] = k_plane_attr_mod_lo[p];
            attrs[i++] = (EGLint)(im->modifier & 0xffffffffULL);
            attrs[i++] = k_plane_attr_mod_hi[p];
            attrs[i++] = (EGLint)((im->modifier >> 32) & 0xffffffffULL);
        }
    }
    attrs[i++] = EGL_NONE;

    EGLImageKHR img = backend->eglCreateImageKHR(
        im->egl_display,
        EGL_NO_CONTEXT,
        EGL_LINUX_DMA_BUF_EXT,
        (EGLClientBuffer)NULL,
        attrs);
    if (img == EGL_NO_IMAGE_KHR) {
        return -EIO;
    }
    *out_image = img;
    return 0;
}

void ww_egl_destroy_image(const ww_egl_backend_t *backend,
                          EGLDisplay display,
                          EGLImageKHR image) {
    if (!backend || !backend->loaded || image == EGL_NO_IMAGE_KHR) return;
    (void)backend->eglDestroyImageKHR(display, image);
}

int ww_egl_texture_from_image(const ww_egl_backend_t *backend,
                              EGLImageKHR image,
                              GLuint *out_tex) {
    if (!backend || !out_tex) return -EINVAL;
    if (!backend->loaded) return -ENOSYS;
    if (image == EGL_NO_IMAGE_KHR) return -EINVAL;

    GLuint tex = 0;
    backend->glGenTextures(1, &tex);
    if (tex == 0) return -EIO;

    /* GL_TEXTURE_2D is required by GL_OES_EGL_image_external
     * and is the only binding target that accepts arbitrary DRM
     * formats (including YUV) without driver reinterpretation. */
    backend->glBindTexture(GL_TEXTURE_2D, tex);
    backend->glTexParameteri(GL_TEXTURE_2D,
                             GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    backend->glTexParameteri(GL_TEXTURE_2D,
                             GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    backend->glTexParameteri(GL_TEXTURE_2D,
                             GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    backend->glTexParameteri(GL_TEXTURE_2D,
                             GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    backend->glEGLImageTargetTexture2DOES(GL_TEXTURE_2D,
                                          (GLeglImageOES)image);
    backend->glBindTexture(GL_TEXTURE_2D, 0);

    *out_tex = tex;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Acquire sync fence                                                 */
/* ------------------------------------------------------------------ */

int ww_egl_wait_sync_fd(const ww_egl_backend_t *backend,
                        EGLDisplay display,
                        int sync_fd) {
    if (!backend || !backend->loaded) return -ENOSYS;
    if (sync_fd < 0) return -EINVAL;

    /* Wrap the dma_fence sync_fd as an EGLSync. The driver dup2s the
     * fd on success, and we transfer ownership to the driver by
     * returning 0. On failure the caller still owns the fd. */
    const EGLint attrs[] = {
        EGL_SYNC_NATIVE_FENCE_FD_ANDROID, (EGLint)sync_fd,
        EGL_NONE,
    };
    EGLSyncKHR sync = backend->eglCreateSyncKHR(
        display, EGL_SYNC_NATIVE_FENCE_ANDROID, attrs);
    if (sync == EGL_NO_SYNC_KHR) {
        return -EIO;
    }

    /* Queue a GPU-side wait on the current context. This does not
     * block the CPU — subsequent draws are serialised after the
     * fence on the driver timeline. */
    EGLint waited = backend->eglWaitSyncKHR(display, sync, 0);
    (void)backend->eglDestroySyncKHR(display, sync);
    if (waited != EGL_TRUE) {
        /* On failure EGL did not wrap the fd (per EGL_ANDROID_native_fence_sync
         * §3.8.2.4): "If eglCreateSyncKHR fails, the fd attribute is
         * unchanged; otherwise EGL takes ownership". Since we already
         * succeeded in eglCreateSyncKHR, ownership has transferred
         * regardless of eglWaitSyncKHR's return value. So we must NOT
         * double-close here. */
        return -EIO;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  DRM render-node introspection                                      */
/* ------------------------------------------------------------------ */

/* EGL_EXT_device_query / EGL_EXT_device_drm — runtime-resolved here so
 * we don't add unconditional dependencies on any specific EGL version. */
typedef EGLBoolean (EGLAPIENTRYP PFNEGLQUERYDISPLAYATTRIBEXTPROC)(
    EGLDisplay dpy, EGLint attribute, EGLAttrib *value);
typedef const char *(EGLAPIENTRYP PFNEGLQUERYDEVICESTRINGEXTPROC)(
    EGLDeviceEXT device, EGLint name);

#ifndef EGL_DEVICE_EXT
#define EGL_DEVICE_EXT 0x322C
#endif
#ifndef EGL_DRM_DEVICE_FILE_EXT
#define EGL_DRM_DEVICE_FILE_EXT 0x3233
#endif
#ifndef EGL_DRM_RENDER_NODE_FILE_EXT
#define EGL_DRM_RENDER_NODE_FILE_EXT 0x3377
#endif

#include <sys/stat.h>
#include <sys/sysmacros.h>

int ww_egl_query_drm_render_node(const ww_egl_backend_t *backend,
                                 EGLDisplay egl_display,
                                 uint32_t *out_major,
                                 uint32_t *out_minor) {
    if (!backend || !backend->loaded || !out_major || !out_minor) return -EINVAL;
    if (egl_display == EGL_NO_DISPLAY) return -EINVAL;

    PFNEGLQUERYDISPLAYATTRIBEXTPROC qda =
        (PFNEGLQUERYDISPLAYATTRIBEXTPROC)backend->eglGetProcAddress("eglQueryDisplayAttribEXT");
    PFNEGLQUERYDEVICESTRINGEXTPROC qds =
        (PFNEGLQUERYDEVICESTRINGEXTPROC)backend->eglGetProcAddress("eglQueryDeviceStringEXT");
    if (!qda || !qds) return -ENOSYS;

    EGLAttrib dev_attr = 0;
    if (!qda(egl_display, EGL_DEVICE_EXT, &dev_attr)) {
        return -ENOSYS;
    }
    EGLDeviceEXT dev = (EGLDeviceEXT)(uintptr_t)dev_attr;

    /* Prefer the render-node path if the driver advertises it; fall
     * back to the primary node and let the caller stat() to identify
     * the GPU. Both paths land in /dev/dri and st_rdev gives us the
     * (major, minor) pair the daemon needs. */
    const char *path = qds(dev, EGL_DRM_RENDER_NODE_FILE_EXT);
    if (!path) {
        path = qds(dev, EGL_DRM_DEVICE_FILE_EXT);
    }
    if (!path) return -ENOSYS;

    struct stat st;
    if (stat(path, &st) != 0) return -errno;
    *out_major = major(st.st_rdev);
    *out_minor = minor(st.st_rdev);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Format/modifier capability probe                                   */
/* ------------------------------------------------------------------ */

int ww_egl_query_format_caps(const ww_egl_backend_t *backend,
                             EGLDisplay egl_display,
                             ww_egl_caps_emit_fn emit,
                             void *user_data) {
    if (!backend || !backend->loaded || !emit) return -EINVAL;
    if (!backend->eglQueryDmaBufFormatsEXT
        || !backend->eglQueryDmaBufModifiersEXT) {
        return -ENOSYS;
    }

    /* Step 1: enumerate fourccs the driver imports. */
    EGLint num_fmts = 0;
    if (!backend->eglQueryDmaBufFormatsEXT(egl_display, 0, NULL, &num_fmts)
        || num_fmts <= 0) {
        ww_log(WAYWALLEN_LOG_DEBUG, "egl: eglQueryDmaBufFormatsEXT count=0");
        return -ENOSYS;
    }
    EGLint *fmts = (EGLint *)calloc((size_t)num_fmts, sizeof(*fmts));
    if (!fmts) return -ENOMEM;
    if (!backend->eglQueryDmaBufFormatsEXT(egl_display, num_fmts, fmts, &num_fmts)) {
        free(fmts);
        return -EIO;
    }

    /* Step 2: per fourcc, enumerate modifiers + filter external_only.
     * Skip fourccs that report 0 modifiers — they're typically formats
     * the driver lists but can't actually import (NVIDIA/Mesa quirk). */
    int worst_rc = 0;
    for (EGLint i = 0; i < num_fmts; ++i) {
        EGLint num_mods = 0;
        if (!backend->eglQueryDmaBufModifiersEXT(egl_display, fmts[i], 0,
                                                 NULL, NULL, &num_mods)) {
            continue;
        }
        if (num_mods <= 0) {
            /* Implicit-modifier-only; advertise as LINEAR so the daemon's
             * picker has at least one option per fourcc. */
            emit((uint32_t)fmts[i], 0 /*LINEAR*/, 1, user_data);
            continue;
        }
        EGLuint64KHR *mods = (EGLuint64KHR *)calloc((size_t)num_mods, sizeof(*mods));
        EGLBoolean *ext_only = (EGLBoolean *)calloc((size_t)num_mods, sizeof(*ext_only));
        if (!mods || !ext_only) {
            free(mods);
            free(ext_only);
            worst_rc = -ENOMEM;
            break;
        }
        if (backend->eglQueryDmaBufModifiersEXT(egl_display, fmts[i], num_mods,
                                                mods, ext_only, &num_mods)) {
            for (EGLint j = 0; j < num_mods; ++j) {
                if (ext_only[j]) continue;   /* skip GL_TEXTURE_EXTERNAL-only */
                emit((uint32_t)fmts[i], (uint64_t)mods[j], 1, user_data);
            }
        }
        free(mods);
        free(ext_only);
    }
    free(fmts);
    return worst_rc;
}

#else /* !WW_HAVE_EGL */

/* Silence -Wempty-translation-unit when the backend is disabled. */
typedef int ww_egl_backend_disabled_t;

#endif /* WW_HAVE_EGL */
