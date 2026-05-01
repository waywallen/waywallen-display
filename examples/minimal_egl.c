/*
 * minimal_egl.c — headless GBM+EGL consumer for waywallen-display-v1.
 *
 * Initialises a surfaceless EGL context via GBM on /dev/dri/renderD128,
 * connects to the daemon, and drives a dispatch loop that exercises the
 * real DMA-BUF → EGLImage → GL texture import path. Prints each
 * callback event to stderr so the operator can see the flow.
 *
 * Usage:
 *   minimal_egl [--socket PATH] [--render-node PATH]
 *               [--max-frames N] [--name STR]
 *
 * Requires: libEGL, libGLESv2, libgbm, a DRI render node.
 */

#define _POSIX_C_SOURCE 200809L

#include "waywallen_display.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <errno.h>
#include <fcntl.h>
#include <gbm.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* EGL_PLATFORM_GBM_KHR / _MESA — use whichever the headers define. */
#ifndef EGL_PLATFORM_GBM_KHR
#  ifdef EGL_PLATFORM_GBM_MESA
#    define EGL_PLATFORM_GBM_KHR EGL_PLATFORM_GBM_MESA
#  else
#    define EGL_PLATFORM_GBM_KHR 0x31D7
#  endif
#endif

struct state {
    int64_t frames_seen;
    int64_t max_frames;
    int disconnected;
    uint32_t last_idx;
    uint64_t last_seq;
    int textures_valid;
};

static void on_textures_ready(void *ud, const waywallen_textures_t *t) {
    struct state *s = (struct state *)ud;
    s->textures_valid = (t->backend == WAYWALLEN_BACKEND_EGL && t->gl_textures != NULL);
    fprintf(stderr,
        "[egl] textures_ready: count=%u %ux%u backend=%d gl_textures=%s\n",
        t->count, t->tex_width, t->tex_height, (int)t->backend,
        t->gl_textures ? "YES" : "NULL");
    if (t->gl_textures) {
        for (uint32_t i = 0; i < t->count; i++) {
            fprintf(stderr, "  tex[%u] = GL name %u\n", i, t->gl_textures[i]);
        }
    }
}

static void on_textures_releasing(void *ud, const waywallen_textures_t *t) {
    struct state *s = (struct state *)ud;
    s->textures_valid = 0;
    fprintf(stderr, "[egl] textures_releasing: count=%u\n", t->count);
}

static void on_config(void *ud, const waywallen_config_t *c) {
    (void)ud;
    fprintf(stderr,
        "[egl] config: src=(%.0f,%.0f,%.0f,%.0f) dst=(%.0f,%.0f,%.0f,%.0f)\n",
        (double)c->source_rect.x, (double)c->source_rect.y,
        (double)c->source_rect.w, (double)c->source_rect.h,
        (double)c->dest_rect.x, (double)c->dest_rect.y,
        (double)c->dest_rect.w, (double)c->dest_rect.h);
}

static void on_frame_ready(void *ud, const waywallen_frame_t *f) {
    struct state *s = (struct state *)ud;
    s->frames_seen++;
    s->last_idx = f->buffer_index;
    s->last_seq = f->seq;
    fprintf(stderr,
        "[egl] frame #%lld: idx=%u seq=%llu (sync already waited by library)\n",
        (long long)s->frames_seen, f->buffer_index,
        (unsigned long long)f->seq);
    // Signal the per-frame release_syncobj so the daemon's reaper sees
    // a real release instead of timing out and force-signaling. This
    // demo doesn't actually GPU-render the texture, so signaling
    // immediately is correct: there is no later "I'm done with this
    // buffer" moment to defer to. The helper closes the fd in all
    // paths.
    if (f->release_syncobj_fd >= 0) {
        (void)waywallen_display_signal_release_syncobj(
            f->release_syncobj_fd);
    }
}

static void on_disconnected(void *ud, int code, const char *msg) {
    struct state *s = (struct state *)ud;
    s->disconnected = 1;
    fprintf(stderr, "[egl] disconnected: code=%d msg=%s\n",
            code, msg ? msg : "(null)");
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);

    int exit_code = 1;
    const char *socket_path = NULL;
    const char *render_node = "/dev/dri/renderD128";
    const char *name = "minimal-egl";
    int64_t max_frames = 5;

    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "--socket") == 0 || strcmp(argv[i], "--display-sock") == 0)
            && i + 1 < argc) {
            socket_path = argv[++i];
        } else if (strcmp(argv[i], "--render-node") == 0 && i + 1 < argc) {
            render_node = argv[++i];
        } else if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
            name = argv[++i];
        } else if (strcmp(argv[i], "--max-frames") == 0 && i + 1 < argc) {
            max_frames = atoll(argv[++i]);
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            fprintf(stderr,
                "usage: minimal_egl [--socket PATH] [--render-node PATH] "
                "[--max-frames N] [--name STR]\n");
            return 0;
        }
    }

    /* ---- GBM + EGL init ---- */
    int drm_fd = open(render_node, O_RDWR | O_CLOEXEC);
    if (drm_fd < 0) {
        perror("open render node");
        return 1;
    }
    struct gbm_device *gbm = gbm_create_device(drm_fd);
    if (!gbm) {
        fprintf(stderr, "gbm_create_device failed\n");
        close(drm_fd);
        return 1;
    }

    EGLDisplay egl_dpy = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR,
                                                (void *)gbm, NULL);
    if (egl_dpy == EGL_NO_DISPLAY) {
        fprintf(stderr, "eglGetPlatformDisplay failed\n");
        gbm_device_destroy(gbm);
        close(drm_fd);
        return 1;
    }
    EGLint major, minor;
    if (!eglInitialize(egl_dpy, &major, &minor)) {
        fprintf(stderr, "eglInitialize failed\n");
        gbm_device_destroy(gbm);
        close(drm_fd);
        return 1;
    }
    fprintf(stderr, "EGL %d.%d on %s\n", major, minor,
            eglQueryString(egl_dpy, EGL_VENDOR));

    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        fprintf(stderr, "eglBindAPI(GLES) failed\n");
        goto cleanup_egl;
    }
    EGLint cfg_attribs[] = {
        EGL_SURFACE_TYPE, 0,  /* surfaceless */
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NONE,
    };
    EGLConfig config;
    EGLint num_cfg;
    if (!eglChooseConfig(egl_dpy, cfg_attribs, &config, 1, &num_cfg)
        || num_cfg == 0) {
        fprintf(stderr, "eglChooseConfig failed\n");
        goto cleanup_egl;
    }
    EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = eglCreateContext(egl_dpy, config, EGL_NO_CONTEXT,
                                     ctx_attribs);
    if (ctx == EGL_NO_CONTEXT) {
        fprintf(stderr, "eglCreateContext failed\n");
        goto cleanup_egl;
    }
    if (!eglMakeCurrent(egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
        fprintf(stderr, "eglMakeCurrent failed\n");
        eglDestroyContext(egl_dpy, ctx);
        goto cleanup_egl;
    }
    fprintf(stderr, "GL: %s / %s\n",
            glGetString(GL_RENDERER), glGetString(GL_VERSION));

    /* ---- waywallen_display ---- */
    struct state st = {
        .frames_seen = 0,
        .max_frames = max_frames,
        .disconnected = 0,
        .last_idx = 0,
        .last_seq = 0,
        .textures_valid = 0,
    };
    waywallen_display_callbacks_t cb = {
        .on_textures_ready = on_textures_ready,
        .on_textures_releasing = on_textures_releasing,
        .on_config = on_config,
        .on_frame_ready = on_frame_ready,
        .on_disconnected = on_disconnected,
        .user_data = &st,
    };
    waywallen_display_t *d = waywallen_display_new(&cb);
    if (!d) {
        fprintf(stderr, "waywallen_display_new failed\n");
        goto cleanup_ctx;
    }
    waywallen_egl_ctx_t egl_ctx = {
        .egl_display = egl_dpy,
        .get_proc_address = NULL,
    };
    int rc = waywallen_display_bind_egl(d, &egl_ctx);
    if (rc != WAYWALLEN_OK) {
        fprintf(stderr, "bind_egl failed: %d\n", rc);
        waywallen_display_destroy(d);
        goto cleanup_ctx;
    }
    fprintf(stderr, "connecting to %s...\n",
            socket_path ? socket_path
                        : "$XDG_RUNTIME_DIR/waywallen/display.sock");

    /* Async handshake — same shape as a GUI host using QSocketNotifier:
     * begin_connect kicks things off non-blocking, advance_handshake is
     * driven by poll readiness until DONE. The legacy waywallen_display_connect
     * would do the same blockingly in one call. */
    rc = waywallen_display_begin_connect(d, socket_path, name, 640, 480, 60000);
    if (rc != WAYWALLEN_OK) {
        fprintf(stderr, "begin_connect failed: %d\n", rc);
        waywallen_display_destroy(d);
        goto cleanup_ctx;
    }
    int ww_fd = waywallen_display_get_fd(d);
    while (1) {
        rc = waywallen_display_advance_handshake(d);
        if (rc == WAYWALLEN_HS_DONE) break;
        if (rc < 0) {
            fprintf(stderr, "handshake failed: %d\n", rc);
            waywallen_display_destroy(d);
            goto cleanup_ctx;
        }
        struct pollfd hp = { .fd = ww_fd, .events = 0, .revents = 0 };
        if (rc == WAYWALLEN_HS_NEED_READ)       hp.events = POLLIN;
        else if (rc == WAYWALLEN_HS_NEED_WRITE) hp.events = POLLOUT;
        else                                    hp.events = POLLIN | POLLOUT;
        int pn = poll(&hp, 1, -1);
        if (pn < 0 && errno != EINTR) { perror("poll handshake"); goto cleanup_ctx; }
    }
    fprintf(stderr, "connected; dispatch loop...\n");
    while (!st.disconnected) {
        struct pollfd pfd = { .fd = ww_fd, .events = POLLIN, .revents = 0 };
        int pr = poll(&pfd, 1, 2000);
        if (pr < 0) { perror("poll"); break; }
        if (pr == 0) { fprintf(stderr, "poll timeout\n"); break; }
        if (pfd.revents & (POLLERR | POLLHUP)) break;
        if (pfd.revents & POLLIN) {
            int r = waywallen_display_dispatch(d);
            if (r < 0) break;
            if (st.frames_seen > 0) {
                waywallen_display_release_frame(d, st.last_idx, st.last_seq);
            }
            if (st.max_frames > 0 && st.frames_seen >= st.max_frames) {
                fprintf(stderr, "max-frames reached\n");
                break;
            }
        }
    }

    if (st.textures_valid) {
        exit_code = 0;
        fprintf(stderr, "SUCCESS: received real GL textures via EGL DMA-BUF import\n");
    } else if (st.frames_seen > 0) {
        fprintf(stderr, "WARNING: frames received but textures were backend=NONE (import failed?)\n");
        exit_code = 1;
    } else {
        fprintf(stderr, "WARNING: no frames received\n");
        exit_code = 1;
    }

    waywallen_display_disconnect(d);
    waywallen_display_destroy(d);

cleanup_ctx:
    eglMakeCurrent(egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(egl_dpy, ctx);
cleanup_egl:
    eglTerminate(egl_dpy);
    gbm_device_destroy(gbm);
    close(drm_fd);
    return exit_code;
}
