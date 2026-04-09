/*
 * Compile-only smoke test for the EGL backend loader.
 *
 * Feeds `ww_egl_backend_load` a host-provided proc address callback
 * that returns a single sentinel function pointer for every symbol
 * query. That short-circuits the `dlsym(libEGL.so.1)` fallback, so
 * the loader's control flow is exercised without requiring a real
 * EGL driver on the test machine.
 *
 * A second test case exercises the dlopen fallback path with a null
 * host callback; it is skipped (without failing) if the environment
 * lacks libEGL or the driver is missing one of the required
 * extensions.
 *
 * Deliberately does NOT call `eglInitialize`, `eglCreateImageKHR`,
 * or any other real EGL/GL function — runtime validation requires a
 * GPU + display and is out of Phase 3 scope.
 */

#define _GNU_SOURCE

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stddef.h>
#include <string.h>

#ifdef WW_HAVE_EGL

#include "backend_egl.h"

/* Non-trivial sentinel: a real function address so the returned
 * `void *` is genuinely callable (if anything ever tried, it'd just
 * return — harmless). The union cast keeps -Wpedantic happy. */
static void sentinel_fn(void) {}

static int mock_call_count = 0;

static void *mock_get_proc_address(const char *name) {
    (void)name;
    mock_call_count++;
    union { void *obj; void (*func)(void); } c;
    c.func = &sentinel_fn;
    return c.obj;
}

static void *null_get_proc_address(const char *name) {
    (void)name;
    return NULL;
}

static void test_mock_loader(void) {
    mock_call_count = 0;
    ww_egl_backend_t backend;
    memset(&backend, 0, sizeof(backend));

    int rc = ww_egl_backend_load(&backend, mock_get_proc_address);
    assert(rc == 0);
    assert(backend.loaded);

    /* Every slot must be populated (all point at the sentinel). */
    assert(backend.eglGetProcAddress != NULL);
    assert(backend.eglInitialize != NULL);
    assert(backend.eglQueryString != NULL);
    assert(backend.eglCreateImageKHR != NULL);
    assert(backend.eglDestroyImageKHR != NULL);
    assert(backend.eglCreateSyncKHR != NULL);
    assert(backend.eglDestroySyncKHR != NULL);
    assert(backend.eglWaitSyncKHR != NULL);
    assert(backend.eglClientWaitSyncKHR != NULL);
    assert(backend.eglDupNativeFenceFDANDROID != NULL);
    assert(backend.glEGLImageTargetTexture2DOES != NULL);
    assert(backend.glGenTextures != NULL);
    assert(backend.glDeleteTextures != NULL);
    assert(backend.glBindTexture != NULL);
    assert(backend.glTexParameteri != NULL);

    /* The loader asks for each required symbol at least once. We
     * resolve around 14 symbols, so the mock should have been hit at
     * least that many times. */
    assert(mock_call_count >= 14);

    ww_egl_backend_unload(&backend);
    assert(!backend.loaded);
    printf("  ok test_mock_loader (%d symbol queries)\n", mock_call_count);
}

static void test_dlopen_fallback(void) {
    ww_egl_backend_t backend;
    memset(&backend, 0, sizeof(backend));

    int rc = ww_egl_backend_load(&backend, null_get_proc_address);
    if (rc == -ENOENT) {
        printf("  skip test_dlopen_fallback (libEGL.so.1 not found)\n");
        return;
    }
    if (rc == -ENOSYS) {
        printf("  skip test_dlopen_fallback (driver missing required ext)\n");
        return;
    }
    assert(rc == 0);
    assert(backend.loaded);
    assert(backend.eglGetProcAddress != NULL);
    assert(backend.eglCreateImageKHR != NULL);
    ww_egl_backend_unload(&backend);
    printf("  ok test_dlopen_fallback\n");
}

int main(void) {
    printf("test_backend_egl:\n");
    test_mock_loader();
    test_dlopen_fallback();
    printf("test_backend_egl: OK\n");
    return 0;
}

#else  /* !WW_HAVE_EGL */

int main(void) {
    printf("test_backend_egl: SKIP (WW_HAVE_EGL not set)\n");
    return 0;
}

#endif
