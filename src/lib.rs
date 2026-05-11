//! Low-level FFI bindings for `libwaywallen_display`.
//!
//! Mirrors `include/waywallen_display.h`. The C library is compiled
//! statically by `build.rs` via the `cc` crate and linked into this
//! crate; consumers don't need a system-installed `libwaywallen_display.so`.
//!
//! GPU handles (`EGLDisplay`, `EGLImageKHR`, `VkInstance`, `VkImage`, …)
//! are typed as `*mut c_void` here, matching the C API. Higher-level
//! crates can re-cast them to the appropriate `ash` / `khronos-egl`
//! types as needed.
//!
//! All function calls are unsafe; thread-safety rules and ownership
//! semantics are documented in the C header — read it before binding
//! these into a safe wrapper.

#![allow(non_camel_case_types, non_snake_case)]

use core::ffi::{c_char, c_int, c_void};

pub const WAYWALLEN_DISPLAY_VERSION_MAJOR: u32 = 0;
pub const WAYWALLEN_DISPLAY_VERSION_MINOR: u32 = 1;
pub const WAYWALLEN_DISPLAY_PROTOCOL_VERSION: u32 = 5;

// -----------------------------------------------------------------------------
// Return codes
// -----------------------------------------------------------------------------

pub type waywallen_err_t = c_int;
pub const WAYWALLEN_OK: waywallen_err_t = 0;
pub const WAYWALLEN_ERR_INVAL: waywallen_err_t = -1;
pub const WAYWALLEN_ERR_NOMEM: waywallen_err_t = -2;
pub const WAYWALLEN_ERR_STATE: waywallen_err_t = -3;
pub const WAYWALLEN_ERR_IO: waywallen_err_t = -4;
pub const WAYWALLEN_ERR_PROTO: waywallen_err_t = -5;
pub const WAYWALLEN_ERR_NOTCONN: waywallen_err_t = -6;
pub const WAYWALLEN_ERR_NOT_IMPL: waywallen_err_t = -7;

// -----------------------------------------------------------------------------
// Logging
// -----------------------------------------------------------------------------

pub type waywallen_log_level_t = c_int;
pub const WAYWALLEN_LOG_DEBUG: waywallen_log_level_t = 0;
pub const WAYWALLEN_LOG_INFO: waywallen_log_level_t = 1;
pub const WAYWALLEN_LOG_WARN: waywallen_log_level_t = 2;
pub const WAYWALLEN_LOG_ERROR: waywallen_log_level_t = 3;

pub type waywallen_log_callback_t = Option<
    unsafe extern "C" fn(
        level: waywallen_log_level_t,
        msg: *const c_char,
        user_data: *mut c_void,
    ),
>;

// -----------------------------------------------------------------------------
// Connection / handshake / stream state
// -----------------------------------------------------------------------------

pub type waywallen_conn_state_t = c_int;
pub const WAYWALLEN_CONN_DISCONNECTED: waywallen_conn_state_t = 0;
pub const WAYWALLEN_CONN_CONNECTING: waywallen_conn_state_t = 1;
pub const WAYWALLEN_CONN_CONNECTED: waywallen_conn_state_t = 2;
pub const WAYWALLEN_CONN_DEAD: waywallen_conn_state_t = 3;

pub type waywallen_stream_state_t = c_int;
pub const WAYWALLEN_STREAM_INACTIVE: waywallen_stream_state_t = 0;
pub const WAYWALLEN_STREAM_ACTIVE: waywallen_stream_state_t = 1;

pub type waywallen_handshake_state_t = c_int;
pub const WAYWALLEN_HS_IDLE: waywallen_handshake_state_t = 0;
pub const WAYWALLEN_HS_CONNECTING: waywallen_handshake_state_t = 1;
pub const WAYWALLEN_HS_HELLO_PENDING: waywallen_handshake_state_t = 2;
pub const WAYWALLEN_HS_WELCOME_WAIT: waywallen_handshake_state_t = 3;
pub const WAYWALLEN_HS_REGISTER_PEND: waywallen_handshake_state_t = 4;
pub const WAYWALLEN_HS_ACCEPTED_WAIT: waywallen_handshake_state_t = 5;
pub const WAYWALLEN_HS_READY: waywallen_handshake_state_t = 6;

// Action codes returned from waywallen_display_advance_handshake().
pub const WAYWALLEN_HS_DONE: c_int = 1;
pub const WAYWALLEN_HS_NEED_READ: c_int = 2;
pub const WAYWALLEN_HS_NEED_WRITE: c_int = 3;
pub const WAYWALLEN_HS_PROGRESS: c_int = 4;

// -----------------------------------------------------------------------------
// Backends
// -----------------------------------------------------------------------------

pub type waywallen_backend_t = c_int;
pub const WAYWALLEN_BACKEND_NONE: waywallen_backend_t = 0;
pub const WAYWALLEN_BACKEND_EGL: waywallen_backend_t = 1;
pub const WAYWALLEN_BACKEND_VULKAN: waywallen_backend_t = 2;

#[repr(C)]
#[derive(Copy, Clone)]
pub struct waywallen_egl_ctx_t {
    pub egl_display: *mut c_void,
    pub get_proc_address: Option<unsafe extern "C" fn(name: *const c_char) -> *mut c_void>,
}

#[repr(C)]
#[derive(Copy, Clone)]
pub struct waywallen_vk_ctx_t {
    pub instance: *mut c_void,
    pub physical_device: *mut c_void,
    pub device: *mut c_void,
    pub queue_family_index: u32,
    pub vk_get_instance_proc_addr:
        Option<unsafe extern "C" fn(instance: *mut c_void, name: *const c_char) -> *mut c_void>,
}

// -----------------------------------------------------------------------------
// Callback payloads
// -----------------------------------------------------------------------------

#[repr(C)]
#[derive(Copy, Clone, Debug, Default)]
pub struct waywallen_rect_t {
    pub x: f32,
    pub y: f32,
    pub w: f32,
    pub h: f32,
}

#[repr(C)]
pub struct waywallen_textures_t {
    pub count: u32,
    pub tex_width: u32,
    pub tex_height: u32,
    pub fourcc: u32,
    pub modifier: u64,
    pub planes_per_buffer: u32,

    pub backend: waywallen_backend_t,
    pub egl_images: *mut *mut c_void,
    pub gl_textures: *mut u32,
    pub vk_images: *mut *mut c_void,
    pub vk_memories: *mut *mut c_void,
}

#[repr(C)]
pub struct waywallen_config_t {
    pub source_rect: waywallen_rect_t,
    pub dest_rect: waywallen_rect_t,
    pub transform: u32,
    pub clear_color: [f32; 4],
}

#[repr(C)]
pub struct waywallen_frame_t {
    pub buffer_index: u32,
    pub seq: u64,
    pub vk_acquire_semaphore: *mut c_void,
    pub release_syncobj_fd: c_int,
}

// -----------------------------------------------------------------------------
// Callback table
// -----------------------------------------------------------------------------

#[repr(C)]
#[derive(Copy, Clone)]
pub struct waywallen_display_callbacks_t {
    pub on_textures_ready: Option<
        unsafe extern "C" fn(user_data: *mut c_void, textures: *const waywallen_textures_t),
    >,
    pub on_textures_releasing: Option<
        unsafe extern "C" fn(user_data: *mut c_void, textures: *const waywallen_textures_t),
    >,
    pub on_config: Option<
        unsafe extern "C" fn(user_data: *mut c_void, config: *const waywallen_config_t),
    >,
    pub on_frame_ready: Option<
        unsafe extern "C" fn(user_data: *mut c_void, frame: *const waywallen_frame_t),
    >,
    pub on_disconnected: Option<
        unsafe extern "C" fn(user_data: *mut c_void, err_code: c_int, message: *const c_char),
    >,
    pub user_data: *mut c_void,
}

// -----------------------------------------------------------------------------
// Opaque handle
// -----------------------------------------------------------------------------

#[repr(C)]
pub struct waywallen_display {
    _opaque: [u8; 0],
    _marker: core::marker::PhantomData<(*mut u8, core::marker::PhantomPinned)>,
}
pub type waywallen_display_t = waywallen_display;

// -----------------------------------------------------------------------------
// Functions
// -----------------------------------------------------------------------------

extern "C" {
    pub fn waywallen_display_set_log_callback(
        cb: waywallen_log_callback_t,
        user_data: *mut c_void,
    );

    pub fn waywallen_display_new(
        cb: *const waywallen_display_callbacks_t,
    ) -> *mut waywallen_display_t;

    /// Async tear-down primitives. `close` is any-thread, `drain` is
    /// the render-thread step that destroys backend resources queued
    /// by callbacks, `free` releases the handle. `shutdown` is the
    /// same-thread convenience that does close + drain + free.
    pub fn waywallen_display_close(d: *mut waywallen_display_t);
    pub fn waywallen_display_drain(d: *mut waywallen_display_t) -> c_int;
    pub fn waywallen_display_free(d: *mut waywallen_display_t);
    pub fn waywallen_display_shutdown(d: *mut waywallen_display_t);

    pub fn waywallen_display_bind_egl(
        d: *mut waywallen_display_t,
        ctx: *const waywallen_egl_ctx_t,
    ) -> c_int;
    pub fn waywallen_display_bind_vulkan(
        d: *mut waywallen_display_t,
        ctx: *const waywallen_vk_ctx_t,
    ) -> c_int;

    pub fn waywallen_display_set_drm_render_node(
        d: *mut waywallen_display_t,
        major: u32,
        minor: u32,
    ) -> c_int;

    pub fn waywallen_display_begin_connect(
        d: *mut waywallen_display_t,
        socket_path: *const c_char,
        display_name: *const c_char,
        instance_id: *const c_char,
        width: u32,
        height: u32,
        refresh_mhz: u32,
    ) -> c_int;

    pub fn waywallen_display_advance_handshake(d: *mut waywallen_display_t) -> c_int;

    pub fn waywallen_display_handshake_state(
        d: *mut waywallen_display_t,
    ) -> waywallen_handshake_state_t;

    pub fn waywallen_display_connect(
        d: *mut waywallen_display_t,
        socket_path: *const c_char,
        display_name: *const c_char,
        instance_id: *const c_char,
        width: u32,
        height: u32,
        refresh_mhz: u32,
    ) -> c_int;

    pub fn waywallen_display_update_size(
        d: *mut waywallen_display_t,
        width: u32,
        height: u32,
    ) -> c_int;

    pub fn waywallen_display_get_fd(d: *mut waywallen_display_t) -> c_int;
    pub fn waywallen_display_dispatch(d: *mut waywallen_display_t) -> c_int;

    pub fn waywallen_display_release_frame(
        d: *mut waywallen_display_t,
        buffer_index: u32,
        seq: u64,
    ) -> c_int;

    pub fn waywallen_display_signal_release_syncobj(fd: c_int) -> c_int;

    pub fn waywallen_display_conn_state(d: *mut waywallen_display_t) -> waywallen_conn_state_t;
    pub fn waywallen_display_stream_state(d: *mut waywallen_display_t) -> waywallen_stream_state_t;

    pub fn waywallen_display_create_gl_texture(
        d: *mut waywallen_display_t,
        image_index: u32,
        out_gl_texture: *mut u32,
    ) -> c_int;
    pub fn waywallen_display_delete_gl_texture(d: *mut waywallen_display_t, image_index: u32);
}
