#define _POSIX_C_SOURCE 200809L

#include "waywallen_display.h"

#include "backend_vulkan_blit.h"
#include "codec.h"
#include "ww_proto.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

enum
{
    MAX_IMPORTS   = 1024,
    SHADOW_HANDLE = MAX_IMPORTS + 1
};

/* Keep the public display/protocol flow, replacing only Vulkan allocation
 * and copy entry points so retirement and timeout checks need no driver. */
static struct {
    bool     device_alive;
    bool     in_flight;
    VkResult completion;
    unsigned next_import;
    unsigned images;
    unsigned memories;
    unsigned semaphores;
    unsigned idle_waits;
    unsigned destroyed_devices;
    int      memory_fds[MAX_IMPORTS];
    bool     image_alive[MAX_IMPORTS];
} gpu;

static VkResult create_semaphore(VkDevice device, const VkSemaphoreCreateInfo* info,
                                 const VkAllocationCallbacks* allocator, VkSemaphore* out) {
    (void)device;
    (void)info;
    (void)allocator;
    assert(gpu.device_alive);
    *out = (VkSemaphore)(uintptr_t)(++gpu.semaphores);
    return VK_SUCCESS;
}

static void destroy_semaphore(VkDevice device, VkSemaphore semaphore,
                              const VkAllocationCallbacks* allocator) {
    (void)device;
    (void)semaphore;
    (void)allocator;
    assert(gpu.device_alive && ! gpu.in_flight && gpu.semaphores > 0);
    --gpu.semaphores;
}

static void destroy_image(VkDevice device, VkImage image, const VkAllocationCallbacks* allocator) {
    (void)device;
    (void)allocator;
    assert(gpu.device_alive && ! gpu.in_flight);
    unsigned id = (unsigned)(uintptr_t)image;
    if (id == SHADOW_HANDLE) return;
    assert(id > 0 && id <= gpu.next_import && gpu.image_alive[id - 1]);
    gpu.image_alive[id - 1] = false;
    --gpu.images;
}

static void free_memory(VkDevice device, VkDeviceMemory memory,
                        const VkAllocationCallbacks* allocator) {
    (void)device;
    (void)allocator;
    assert(gpu.device_alive && ! gpu.in_flight);
    unsigned id = (unsigned)(uintptr_t)memory;
    if (id == SHADOW_HANDLE) return;
    assert(id > 0 && id <= gpu.next_import && gpu.memory_fds[id - 1] >= 0);
    close(gpu.memory_fds[id - 1]);
    gpu.memory_fds[id - 1] = -1;
    --gpu.memories;
}

static VkResult wait_idle(VkDevice device) {
    (void)device;
    assert(gpu.device_alive);
    ++gpu.idle_waits;
    gpu.in_flight  = false;
    gpu.completion = VK_SUCCESS;
    return VK_SUCCESS;
}

static VkResult wait_fences(VkDevice device, uint32_t count, const VkFence* fences, VkBool32 all,
                            uint64_t timeout) {
    (void)device;
    (void)count;
    (void)fences;
    (void)all;
    assert(gpu.device_alive && timeout == 0);
    return gpu.completion;
}

static VkResult reset_fences(VkDevice device, uint32_t count, const VkFence* fences) {
    (void)device;
    (void)count;
    (void)fences;
    assert(gpu.device_alive && ! gpu.in_flight);
    return VK_SUCCESS;
}

static VkResult import_semaphore(VkDevice device, const VkImportSemaphoreFdInfoKHR* info) {
    (void)device;
    close(info->fd);
    return VK_SUCCESS;
}

static void get_queue(VkDevice device, uint32_t family, uint32_t index, VkQueue* queue) {
    (void)device;
    (void)family;
    (void)index;
    *queue = (VkQueue)(uintptr_t)1;
}

static PFN_vkVoidFunction get_device_proc(VkDevice device, const char* name) {
    (void)device;
    return strcmp(name, "vkGetDeviceQueue") == 0 ? (PFN_vkVoidFunction)get_queue : NULL;
}

int __wrap_ww_vk_create_owned(ww_vk_owned_t* out) {
    assert(! gpu.device_alive);
    memset(&gpu, 0, sizeof(gpu));
    gpu.device_alive = true;
    gpu.completion   = VK_SUCCESS;
    *out             = (ww_vk_owned_t) {
        .instance        = (VkInstance)(uintptr_t)1,
        .physical_device = (VkPhysicalDevice)(uintptr_t)1,
        .device          = (VkDevice)(uintptr_t)1,
        .queue           = (VkQueue)(uintptr_t)1,
    };
    return 0;
}

void __wrap_ww_vk_destroy_owned(ww_vk_owned_t* owned) {
    assert(gpu.device_alive && ! gpu.in_flight);
    assert(gpu.images == 0 && gpu.memories == 0 && gpu.semaphores == 0);
    gpu.device_alive = false;
    ++gpu.destroyed_devices;
    memset(owned, 0, sizeof(*owned));
}

int __wrap_ww_vk_backend_load(ww_vk_backend_t* backend, VkInstance instance,
                              VkPhysicalDevice physical_device, VkDevice device,
                              uint32_t queue_family, ww_vk_get_instance_proc_addr_fn get_proc,
                              bool debug_utils) {
    (void)get_proc;
    (void)debug_utils;
    *backend = (ww_vk_backend_t) {
        .instance               = instance,
        .physical_device        = physical_device,
        .device                 = device,
        .queue_family_index     = queue_family,
        .loaded                 = true,
        .vkGetDeviceProcAddr    = get_device_proc,
        .vkCreateSemaphore      = create_semaphore,
        .vkDestroySemaphore     = destroy_semaphore,
        .vkDestroyImage         = destroy_image,
        .vkFreeMemory           = free_memory,
        .vkDeviceWaitIdle       = wait_idle,
        .vkImportSemaphoreFdKHR = import_semaphore,
    };
    return 0;
}

int __wrap_ww_vk_import_dmabuf(const ww_vk_backend_t* backend, const ww_vk_dmabuf_import_t* input,
                               ww_vk_imported_image_t* out) {
    (void)backend;
    assert(gpu.device_alive && gpu.next_import < MAX_IMPORTS);
    unsigned id             = ++gpu.next_import;
    gpu.image_alive[id - 1] = true;
    gpu.memory_fds[id - 1]  = input->fds[0];
    ++gpu.images;
    ++gpu.memories;
    *out = (ww_vk_imported_image_t) {
        .image  = (VkImage)(uintptr_t)id,
        .memory = (VkDeviceMemory)(uintptr_t)id,
    };
    return 0;
}

int __wrap_ww_vk_blitter_init(ww_vk_blitter_t* blitter, VkInstance instance,
                              VkPhysicalDevice physical_device, VkDevice device,
                              uint32_t queue_family, VkQueue queue,
                              ww_vk_get_instance_proc_addr_fn get_proc) {
    memset(blitter, 0, sizeof(*blitter));
    __wrap_ww_vk_backend_load(
        &blitter->backend, instance, physical_device, device, queue_family, get_proc, false);
    blitter->initialized                = true;
    blitter->queue                      = queue;
    blitter->fence                      = (VkFence)(uintptr_t)1;
    blitter->vkWaitForFences            = wait_fences;
    blitter->vkResetFences              = reset_fences;
    blitter->pending_release_syncobj_fd = -1;
    blitter->shadow_export_fd           = -1;
    return 0;
}

int __wrap_ww_vk_blitter_ensure_shadow_exportable(ww_vk_blitter_t* blitter, uint32_t width,
                                                  uint32_t height, VkFormat format) {
    blitter->shadow_image             = (VkImage)(uintptr_t)SHADOW_HANDLE;
    blitter->shadow_mem               = (VkDeviceMemory)(uintptr_t)SHADOW_HANDLE;
    blitter->shadow_w                 = width;
    blitter->shadow_h                 = height;
    blitter->shadow_fmt               = format;
    blitter->shadow_export_n_planes   = 1;
    blitter->shadow_export_strides[0] = width * 4;
    if (blitter->shadow_export_fd < 0) {
        blitter->shadow_export_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
        assert(blitter->shadow_export_fd >= 0);
    }
    return 0;
}

int __wrap_ww_vk_blitter_blit(ww_vk_blitter_t* blitter, VkImage image, uint32_t width,
                              uint32_t height, VkSemaphore acquire, int release_fd, bool* armed) {
    (void)image;
    (void)width;
    (void)height;
    (void)acquire;
    if (release_fd >= 0) close(release_fd);
    *armed               = true;
    blitter->fence_armed = gpu.completion != VK_SUCCESS;
    gpu.in_flight        = blitter->fence_armed;
    return gpu.in_flight ? -ETIMEDOUT : 0;
}

struct session {
    waywallen_display_t* display;
    int                  peer;
};

static void receive_request(int peer, uint16_t expected) {
    uint8_t  bytes[WW_CODEC_MAX_BODY_BYTES];
    uint16_t opcode;
    size_t   size, fd_count;
    int      fds[4];
    assert(ww_codec_recv_request(peer, &opcode, bytes, sizeof(bytes), &size, fds, 4, &fd_count) ==
           0);
    assert(opcode == expected && fd_count == 0);
}

static void send_event(int peer, uint16_t opcode, ww_buf_t* body, int* fds, size_t fd_count) {
    assert(ww_codec_send_event(peer, opcode, body->data, body->len, fds, fd_count) == 0);
    ww_buf_free(body);
}

static struct session start_session(bool relay) {
    waywallen_display_callbacks_t callbacks = { 0 };
    struct session session = { .display = waywallen_display_new(&callbacks), .peer = -1 };
    assert(session.display);
    if (relay) {
        assert(waywallen_display_bind_dmabuf_relay(session.display) == WAYWALLEN_OK);
    } else {
        ww_vk_owned_t owned;
        assert(__wrap_ww_vk_create_owned(&owned) == 0);
        waywallen_vk_ctx_t context = {
            .instance        = (void*)owned.instance,
            .physical_device = (void*)owned.physical_device,
            .device          = (void*)owned.device,
        };
        assert(waywallen_display_bind_vulkan(session.display, &context) == WAYWALLEN_OK);
    }

    char directory[] = "/tmp/waywallen-relay-XXXXXX";
    assert(mkdtemp(directory));
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    snprintf(address.sun_path, sizeof(address.sun_path), "%s/socket", directory);
    int listener = socket(AF_UNIX, SOCK_STREAM, 0);
    assert(listener >= 0);
    assert(bind(listener, (const struct sockaddr*)&address, sizeof(address)) == 0);
    assert(listen(listener, 1) == 0);
    const waywallen_display_metrics_t metrics = { .width = 64, .height = 64, .refresh_mhz = 60000 };
    assert(waywallen_display_begin_connect(
               session.display, address.sun_path, "relay-test", NULL, &metrics) == WAYWALLEN_OK);
    session.peer = accept(listener, NULL, NULL);
    assert(session.peer >= 0);
    close(listener);
    unlink(address.sun_path);
    rmdir(directory);

    assert(waywallen_display_advance_handshake(session.display) == WAYWALLEN_HS_NEED_READ);
    receive_request(session.peer, WW_REQ_HELLO);
    ww_evt_welcome_t welcome = { .server_version = (char*)"relay-test" };
    ww_buf_t         body;
    ww_buf_init(&body);
    assert(ww_evt_welcome_encode(&welcome, &body) == WW_OK);
    send_event(session.peer, WW_EVT_WELCOME, &body, NULL, 0);
    assert(waywallen_display_advance_handshake(session.display) == WAYWALLEN_HS_NEED_READ);
    receive_request(session.peer, WW_REQ_CLIENT_CAPABILITIES);
    receive_request(session.peer, WW_REQ_REGISTER_DISPLAY);
    ww_evt_display_accepted_t accepted                    = { .display_id = 1 };
    accepted.presentation.config.generation               = 1;
    accepted.presentation.config.pause_effect.blur.radius = 30;
    accepted.presentation.config.transition.duration_ms   = 500;
    accepted.presentation.config.transition.origin_x      = 0.5f;
    accepted.presentation.config.transition.origin_y      = 0.5f;
    accepted.presentation.state.generation                = 1;
    accepted.presentation.state.config_generation         = 1;
    ww_buf_init(&body);
    assert(ww_evt_display_accepted_encode(&accepted, &body) == WW_OK);
    send_event(session.peer, WW_EVT_DISPLAY_ACCEPTED, &body, NULL, 0);
    assert(waywallen_display_advance_handshake(session.display) == WAYWALLEN_HS_DONE);
    return session;
}

static void bind_pool(struct session* session, uint64_t generation) {
    uint32_t strides[] = { 256, 256, 256 };
    uint32_t offsets[] = { 0, 0, 0 };
    uint64_t sizes[]   = { 16384, 16384, 16384 };
    int      fds[3];
    for (unsigned i = 0; i < 3; ++i) {
        fds[i] = open("/dev/null", O_RDONLY | O_CLOEXEC);
        assert(fds[i] >= 0);
    }
    ww_evt_bind_buffers_t binding = {
        .buffer_generation = generation,
        .count = 3,
        .width = 64,
        .height = 64,
        .fourcc = 0x34324241,
        .planes_per_buffer = 1,
        .stride = {.count = 3, .data = strides},
        .plane_offset = {.count = 3, .data = offsets},
        .size = {.count = 3, .data = sizes},
        .initial_config = {
            .generation = generation,
            .buffer_generation = generation,
            .source_rect = {.w = 64, .h = 64},
            .dest_rect = {.w = 64, .h = 64},
            .clear_color = {.a = 1},
        },
        .content_token = generation,
        .presentation_config_generation = 1,
    };
    ww_buf_t body;
    ww_buf_init(&body);
    assert(ww_evt_bind_buffers_encode(&binding, &body) == WW_OK);
    send_event(session->peer, WW_EVT_BIND_BUFFERS, &body, fds, 3);
    for (unsigned i = 0; i < 3; ++i) close(fds[i]);
    assert(waywallen_display_dispatch(session->display) == WAYWALLEN_OK);
}

static void unbind_pool(struct session* session, uint64_t generation) {
    ww_evt_unbind_t unbind = { .buffer_generation = generation };
    ww_buf_t        body;
    ww_buf_init(&body);
    assert(ww_evt_unbind_encode(&unbind, &body) == WW_OK);
    send_event(session->peer, WW_EVT_UNBIND, &body, NULL, 0);
    assert(waywallen_display_dispatch(session->display) == WAYWALLEN_OK);
}

static void submit_copy(struct session* session, uint64_t generation, VkResult completion) {
    ww_evt_frame_ready_t frame = { .buffer_generation = generation, .seq = generation };
    int                  fds[] = { open("/dev/null", O_RDONLY), open("/dev/null", O_RDONLY) };
    assert(fds[0] >= 0 && fds[1] >= 0);
    ww_buf_t body;
    ww_buf_init(&body);
    assert(ww_evt_frame_ready_encode(&frame, &body) == WW_OK);
    send_event(session->peer, WW_EVT_FRAME_READY, &body, fds, 2);
    close(fds[0]);
    close(fds[1]);
    gpu.completion = completion;
    assert(waywallen_display_dispatch(session->display) == WAYWALLEN_OK);
    assert(gpu.in_flight == (completion != VK_SUCCESS));
}

static void finish_session(struct session* session) {
    waywallen_display_shutdown(session->display);
    close(session->peer);
    assert(! gpu.device_alive && gpu.destroyed_devices == 1);
}

static void test_replaced_and_unbound_pools_are_collected(void) {
    struct session session = start_session(true);
    for (uint64_t generation = 1; generation <= 100; ++generation) {
        bind_pool(&session, generation);
        submit_copy(&session, generation, VK_SUCCESS);
        assert(gpu.images == 3 && gpu.memories == 3 && gpu.semaphores == 3);
    }
    assert(gpu.idle_waits == 0);
    unbind_pool(&session, 100);
    assert(gpu.images == 0 && gpu.memories == 0 && gpu.semaphores == 0);
    finish_session(&session);
}

static void test_pending_and_failed_completion_keep_imports(void) {
    struct session session = start_session(true);
    bind_pool(&session, 1);
    submit_copy(&session, 1, VK_TIMEOUT);
    bind_pool(&session, 2);
    assert(gpu.images == 6 && gpu.memories == 6 && gpu.semaphores == 6);
    gpu.completion = VK_ERROR_DEVICE_LOST;
    assert(waywallen_display_dispatch(session.display) == WAYWALLEN_OK);
    assert(gpu.images == 6 && gpu.memories == 6 && gpu.semaphores == 6);
    gpu.completion = VK_SUCCESS;
    gpu.in_flight  = false;
    assert(waywallen_display_dispatch(session.display) == WAYWALLEN_OK);
    assert(gpu.images == 3 && gpu.memories == 3 && gpu.semaphores == 3);
    assert(gpu.idle_waits == 0);
    finish_session(&session);
}

static void test_close_drains_before_destroying_device(void) {
    for (unsigned unbind = 0; unbind < 2; ++unbind) {
        struct session session = start_session(true);
        bind_pool(&session, 1);
        submit_copy(&session, 1, VK_TIMEOUT);
        if (unbind)
            unbind_pool(&session, 1);
        else
            bind_pool(&session, 2);
        const unsigned expected = unbind ? 3 : 6;
        assert(gpu.images == expected && gpu.memories == expected && gpu.semaphores == expected);
        finish_session(&session);
        assert(gpu.idle_waits == 1);
    }
}

static void test_external_vulkan_still_requires_host_drain(void) {
    struct session session = start_session(false);
    bind_pool(&session, 1);
    bind_pool(&session, 2);
    assert(gpu.images == 6);
    assert(waywallen_display_drain(session.display) == 1);
    assert(gpu.images == 3);
    waywallen_display_close(session.display);
    assert(gpu.images == 3);
    assert(waywallen_display_drain(session.display) == 1);
    waywallen_display_free(session.display);
    close(session.peer);
    ww_vk_owned_t owned = { 0 };
    __wrap_ww_vk_destroy_owned(&owned);
}

static void quiet_log(waywallen_log_level_t level, const char* message, void* data) {
    (void)level;
    (void)message;
    (void)data;
}

int main(void) {
    waywallen_display_set_log_callback(quiet_log, NULL);
    test_replaced_and_unbound_pools_are_collected();
    test_pending_and_failed_completion_keep_imports();
    test_close_drains_before_destroying_device();
    test_external_vulkan_still_requires_host_drain();
    puts("test_dmabuf_relay: OK");
    return 0;
}
