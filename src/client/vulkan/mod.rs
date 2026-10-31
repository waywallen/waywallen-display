mod direct;
mod effects;
mod pipeline;
mod render;
mod resources;
mod runtime;
mod swapchain;
#[cfg(test)]
mod tests;

use self::effects::update_transition_snapshot;
use anyhow::Result;
use ash::vk;
use std::sync::Arc;
use std::time::{Duration, Instant};

const FRAMES_IN_FLIGHT: usize = 2;
const MAX_BLUR_MIP_LEVELS: u32 = 6;
const BLUR_WEIGHT_COUNT: usize = MAX_BLUR_MIP_LEVELS as usize;
const COMPOSITION_PUSH_CONSTANT_SIZE: u32 = std::mem::size_of::<CompositionPushConstants>() as u32;
const BLUR_PUSH_CONSTANT_SIZE: u32 = 8 * 4;
const BLUR_PUSH_CONSTANT_OFFSET: u32 = COMPOSITION_PUSH_CONSTANT_SIZE;
const TRANSITION_PUSH_CONSTANT_SIZE: u32 = 8 * 4;
const TRANSITION_PUSH_CONSTANT_OFFSET: u32 = BLUR_PUSH_CONSTANT_OFFSET + BLUR_PUSH_CONSTANT_SIZE;
const TOTAL_PUSH_CONSTANT_SIZE: u32 =
    TRANSITION_PUSH_CONSTANT_OFFSET + TRANSITION_PUSH_CONSTANT_SIZE;
const RESOURCE_RETIRE_TIMEOUT_NS: u64 = 2_000_000_000;
const SWAPCHAIN_ACQUIRE_TIMEOUT_NS: u64 = 1_000_000;
const BLUR_TRANSITION_DURATION: Duration = Duration::from_millis(180);
/// Soft edge of wipe and grow transitions, as a fraction of the travel distance.
const TRANSITION_EDGE_FEATHER: f32 = 0.04;
const TRANSITION_TARGET_COUNT: usize = 2;
const _: () = assert!(COMPOSITION_PUSH_CONSTANT_SIZE == 48);
const _: () = assert!(TOTAL_PUSH_CONSTANT_SIZE <= 128);

include!(concat!(env!("OUT_DIR"), "/layer_shell_shaders.rs"));

#[repr(C)]
struct CompositionPushConstants {
    position_origin: [f32; 4],
    position_axes: [f32; 4],
    uv_origin_scale: [f32; 4],
}

pub struct VulkanRuntime {
    entry: ash::Entry,
    instance: ash::Instance,
    surface_loader: ash::khr::surface::Instance,
    device: ash::Device,
    swapchain_loader: ash::khr::swapchain::Device,
    physical_device: vk::PhysicalDevice,
    graphics_queue_family: u32,
    present_queue_family: u32,
    graphics_queue: vk::Queue,
    present_queue: vk::Queue,
    debug_utils: Option<ash::ext::debug_utils::Instance>,
    debug_messenger: vk::DebugUtilsMessengerEXT,
}

pub use direct::discard_direct_frame;
pub use runtime::WindowSystem;

pub struct Composition {
    pub source: [f32; 4],
    pub destination: [f32; 4],
    pub transform: u32,
    pub clear: [f32; 4],
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct PausePresentation {
    pub configured: bool,
    pub active: bool,
    pub radius: u32,
}

#[derive(Clone, Copy, Debug)]
struct BlurSegment {
    from: f32,
    target: f32,
    started_at: Instant,
}

#[derive(Debug, Default)]
struct BlurTransition {
    current: f32,
    target: f32,
    segment: Option<BlurSegment>,
}

struct BlurSample {
    radius: f32,
    animating: bool,
    finished: bool,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum TransitionShape {
    Fade,
    /// Degrees clockwise in surface space; 0 wipes left to right.
    Wipe {
        angle: u32,
    },
    /// Center in surface uv space.
    Grow {
        origin: [f32; 2],
    },
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct TransitionPresentation {
    pub shape: TransitionShape,
    pub duration: Duration,
}

#[derive(Clone, Copy, Debug)]
struct ActiveTransition {
    presentation: TransitionPresentation,
    started_at: Instant,
}

#[derive(Clone, Copy, Debug, Default)]
struct TransitionSample {
    /// Shape and eased progress while a transition is still animating.
    running: Option<(TransitionShape, f32)>,
    /// The transition ended since the previous present.
    finished: bool,
}

struct BlurResources {
    image: vk::Image,
    memory: vk::DeviceMemory,
    format: vk::Format,
    all_levels_view: vk::ImageView,
    mip_views: Vec<vk::ImageView>,
    render_pass: vk::RenderPass,
    composition_pipeline: vk::Pipeline,
    downsample_pipeline: vk::Pipeline,
    framebuffers: Vec<vk::Framebuffer>,
    blur_pipeline: vk::Pipeline,
    downsample_descriptor_sets: Vec<vk::DescriptorSet>,
    mix_descriptor_set: vk::DescriptorSet,
    extent: vk::Extent2D,
    base_valid: bool,
    pyramid_valid: bool,
    pyramid_dirty: bool,
}

struct TransitionTarget {
    image: vk::Image,
    memory: vk::DeviceMemory,
    view: vk::ImageView,
    framebuffer: vk::Framebuffer,
    /// Binding 1 samples the scene, binding 2 samples this target.
    descriptor_set: vk::DescriptorSet,
}

/// Scene-sized copies of outgoing content. Starting a transition renders what
/// is on screen into the spare target, so interrupting a running transition
/// continues from its current mix instead of jumping.
struct TransitionResources {
    targets: Vec<TransitionTarget>,
    /// Target that holds the outgoing content.
    front: usize,
    /// Renders the on-screen mix of a running transition into a target.
    capture_pipeline: vk::Pipeline,
    /// Renders the plain scene into a target; the outgoing target may be unwritten.
    capture_scene_pipeline: vk::Pipeline,
    output_pipeline: vk::Pipeline,
}

struct DirectBinding {
    generation: u64,
    content_token: u64,
    presentation_config_generation: u64,
    images: Vec<vk::Image>,
    views: Vec<vk::ImageView>,
    format: vk::Format,
    extent: vk::Extent2D,
}

#[derive(Default)]
struct ContentPresentationState {
    bound: Option<u64>,
    submitted: Option<u64>,
    committed: Option<u64>,
}

impl ContentPresentationState {
    fn bind(&mut self, token: u64) -> bool {
        debug_assert_ne!(token, 0);
        self.bound = Some(token);
        let cancels_uncommitted = self.committed == Some(token)
            && self.submitted.is_some_and(|submitted| submitted != token);
        if cancels_uncommitted {
            self.submitted = None;
        }
        cancels_uncommitted
    }

    fn unbind(&mut self) {
        self.bound = None;
    }

    fn should_transition(&self, configured: bool, retained_scene_valid: bool) -> bool {
        configured
            && retained_scene_valid
            && self
                .bound
                .zip(self.committed)
                .is_some_and(|(bound, committed)| bound != committed)
            && self.submitted != self.bound
    }

    fn mark_submitted(&mut self) {
        self.submitted = self.bound;
    }

    fn discard_submitted(&mut self) {
        self.submitted = None;
    }

    fn has_submitted(&self) -> bool {
        self.submitted.is_some()
    }

    fn present_accepted(&mut self) {
        if let Some(token) = self.submitted.take() {
            self.committed = Some(token);
        }
    }

    fn reset(&mut self) {
        *self = Self::default();
    }
}

struct DirectFrame {
    image: vk::Image,
    view: vk::ImageView,
    extent: vk::Extent2D,
    layout: vk::ImageLayout,
    external_queue_family: u32,
    acquire_semaphore: vk::Semaphore,
    release_syncobj_fd: i32,
    buffer_generation: u64,
    seq: u64,
}

struct DirectRelease {
    release_syncobj_fd: i32,
    buffer_generation: u64,
    seq: u64,
}

struct FrameContext {
    command_pool: vk::CommandPool,
    command_buffer: vk::CommandBuffer,
    image_available: vk::Semaphore,
    fence: vk::Fence,
    descriptor_set: vk::DescriptorSet,
    pending_release: Option<DirectRelease>,
}

struct RetiredSwapchain {
    handle: vk::SwapchainKHR,
    present_ready: Vec<vk::Semaphore>,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum BlankState {
    Inactive,
    Pending,
    Committed,
}

impl BlankState {
    fn request(&mut self) -> bool {
        if *self != Self::Inactive {
            return false;
        }
        *self = Self::Pending;
        true
    }

    fn presented(&mut self, blank: bool) {
        *self = if blank {
            Self::Committed
        } else {
            Self::Inactive
        };
    }

    fn invalidate_swapchain(&mut self) {
        if *self == Self::Committed {
            *self = Self::Pending;
        }
    }

    fn abandon(&mut self) {
        *self = Self::Inactive;
    }
}

pub struct WsiPresenter {
    runtime: Arc<VulkanRuntime>,
    surface: vk::SurfaceKHR,
    swapchain: vk::SwapchainKHR,
    format: vk::Format,
    extent: vk::Extent2D,
    images: Vec<vk::Image>,
    present_ready: Vec<vk::Semaphore>,
    image_views: Vec<vk::ImageView>,
    framebuffers: Vec<vk::Framebuffer>,
    render_pass: vk::RenderPass,
    descriptor_set_layout: vk::DescriptorSetLayout,
    descriptor_pool: vk::DescriptorPool,
    pipeline_layout: vk::PipelineLayout,
    pipeline: vk::Pipeline,
    sampler: vk::Sampler,
    frames: Vec<FrameContext>,
    frame_cursor: usize,
    pause_presentation: PausePresentation,
    pause_presentation_initialized: bool,
    blur_transition: BlurTransition,
    blur_resources: Option<BlurResources>,
    pause_blur_available: bool,
    transition_presentation: Option<TransitionPresentation>,
    presentation_config_generation: u64,
    content_presentation: ContentPresentationState,
    prepared_transition: Option<TransitionPresentation>,
    active_transition: Option<ActiveTransition>,
    transition_resources: Option<TransitionResources>,
    transitions_available: bool,
    direct_binding: Option<DirectBinding>,
    pending_direct_frame: Option<DirectFrame>,
    blank_state: BlankState,
    recreate_extent: Option<vk::Extent2D>,
    retired_swapchains: Vec<RetiredSwapchain>,
}

pub enum PresentResult {
    Presented { redraw: bool },
    Pending,
}

impl WsiPresenter {
    pub fn new(
        runtime: Arc<VulkanRuntime>,
        surface: vk::SurfaceKHR,
        extent: (u32, u32),
    ) -> Result<Self> {
        let mut presenter = Self {
            runtime,
            surface,
            swapchain: vk::SwapchainKHR::null(),
            format: vk::Format::UNDEFINED,
            extent: vk::Extent2D::default(),
            images: Vec::new(),
            present_ready: Vec::new(),
            image_views: Vec::new(),
            framebuffers: Vec::new(),
            render_pass: vk::RenderPass::null(),
            descriptor_set_layout: vk::DescriptorSetLayout::null(),
            descriptor_pool: vk::DescriptorPool::null(),
            pipeline_layout: vk::PipelineLayout::null(),
            pipeline: vk::Pipeline::null(),
            sampler: vk::Sampler::null(),
            frames: Vec::with_capacity(FRAMES_IN_FLIGHT),
            frame_cursor: 0,
            pause_presentation: PausePresentation::default(),
            pause_presentation_initialized: false,
            blur_transition: BlurTransition::default(),
            blur_resources: None,
            pause_blur_available: true,
            transition_presentation: None,
            presentation_config_generation: 0,
            content_presentation: ContentPresentationState::default(),
            prepared_transition: None,
            active_transition: None,
            transition_resources: None,
            transitions_available: true,
            direct_binding: None,
            pending_direct_frame: None,
            blank_state: BlankState::Inactive,
            recreate_extent: None,
            retired_swapchains: Vec::new(),
        };
        presenter.initialize(extent)?;
        Ok(presenter)
    }
}

impl WsiPresenter {
    pub fn request_resize(&mut self, width: u32, height: u32) {
        let extent = vk::Extent2D { width, height };
        if self.extent != extent {
            self.recreate_extent = Some(extent);
            self.blank_state.invalidate_swapchain();
        }
    }

    pub fn supports_pause_blur(&self) -> bool {
        self.pause_blur_available
    }

    /// Transitions draw from the same persistent scene as Pause Blur.
    pub fn supports_transitions(&self) -> bool {
        self.pause_blur_available && self.transitions_available
    }

    /// Returns true when the surface must be presented again to drop an
    /// in-flight transition.
    pub fn apply_transition_snapshot(
        &mut self,
        config_generation: u64,
        presentation: Option<TransitionPresentation>,
    ) -> bool {
        self.presentation_config_generation = config_generation;
        update_transition_snapshot(
            &mut self.transition_presentation,
            &mut self.prepared_transition,
            &mut self.active_transition,
            presentation,
        )
    }

    pub fn apply_pause_snapshot(&mut self, presentation: PausePresentation, now: Instant) -> bool {
        let retire_resources = self.pause_presentation.configured
            && !presentation.configured
            && self.blur_resources.is_some();
        let animate = self.pause_presentation_initialized
            && self
                .blur_resources
                .as_ref()
                .is_some_and(|resources| resources.base_valid)
            && self.pause_blur_available;
        self.pause_presentation = presentation;
        self.pause_presentation_initialized = true;
        self.blur_transition
            .set_target(presentation.target_radius(), now, animate)
            || retire_resources
    }

    pub fn apply_pause_state(&mut self, active: bool, now: Instant) -> bool {
        self.pause_presentation.active = active;
        let animate = self.pause_presentation_initialized
            && self
                .blur_resources
                .as_ref()
                .is_some_and(|resources| resources.base_valid)
            && self.pause_blur_available;
        self.blur_transition
            .set_target(self.pause_presentation.target_radius(), now, animate)
    }

    pub fn reset_display_session(&mut self) {
        self.pause_presentation = PausePresentation::default();
        self.pause_presentation_initialized = false;
        self.blur_transition.reset();
        self.transition_presentation = None;
        self.presentation_config_generation = 0;
        self.content_presentation.reset();
        self.prepared_transition = None;
        self.active_transition = None;
        if let Some(resources) = self.blur_resources.as_mut() {
            resources.base_valid = false;
            resources.pyramid_valid = false;
            resources.pyramid_dirty = false;
        }
    }

    pub fn request_blank(&mut self) -> bool {
        self.reset_display_session();
        self.blank_state.request()
    }

    pub fn blank_committed(&self) -> bool {
        self.blank_state == BlankState::Committed
    }

    pub fn abandon_blank(&mut self) {
        self.blank_state.abandon();
    }

    /// Finished transitions stay recorded until a present submits the final
    /// frame, so an early `Pending` return cannot strand a partial mix.
    fn sample_transition(&self, now: Instant) -> TransitionSample {
        let Some(active) = self.active_transition else {
            return TransitionSample::default();
        };
        match active.progress_at(now) {
            Some(progress) => TransitionSample {
                running: Some((active.presentation.shape, progress)),
                finished: false,
            },
            None => TransitionSample {
                running: None,
                finished: true,
            },
        }
    }
}

impl Drop for WsiPresenter {
    fn drop(&mut self) {
        unsafe {
            let _ = self.runtime.device.device_wait_idle();
        }
        if let Some(direct) = self.pending_direct_frame.take() {
            unsafe { libc::close(direct.release_syncobj_fd) };
        }
        if let Some(binding) = self.direct_binding.take() {
            unsafe {
                for view in binding.views {
                    if view != vk::ImageView::null() {
                        self.runtime.device.destroy_image_view(view, None);
                    }
                }
            }
        }
        self.destroy_current_blur_resources();
        self.destroy_swapchain_rendering();
        unsafe {
            if self.swapchain != vk::SwapchainKHR::null() {
                self.runtime
                    .swapchain_loader
                    .destroy_swapchain(self.swapchain, None);
            }
            for semaphore in self.present_ready.drain(..) {
                self.runtime.device.destroy_semaphore(semaphore, None);
            }
            for retired in self.retired_swapchains.drain(..) {
                self.runtime
                    .swapchain_loader
                    .destroy_swapchain(retired.handle, None);
                for semaphore in retired.present_ready {
                    self.runtime.device.destroy_semaphore(semaphore, None);
                }
            }
            for frame in self.frames.drain(..) {
                if let Some(release) = frame.pending_release {
                    libc::close(release.release_syncobj_fd);
                }
                if frame.fence != vk::Fence::null() {
                    self.runtime.device.destroy_fence(frame.fence, None);
                }
                if frame.image_available != vk::Semaphore::null() {
                    self.runtime
                        .device
                        .destroy_semaphore(frame.image_available, None);
                }
                if frame.command_pool != vk::CommandPool::null() {
                    self.runtime
                        .device
                        .destroy_command_pool(frame.command_pool, None);
                }
            }
            if self.sampler != vk::Sampler::null() {
                self.runtime.device.destroy_sampler(self.sampler, None);
            }
            if self.pipeline_layout != vk::PipelineLayout::null() {
                self.runtime
                    .device
                    .destroy_pipeline_layout(self.pipeline_layout, None);
            }
            if self.descriptor_pool != vk::DescriptorPool::null() {
                self.runtime
                    .device
                    .destroy_descriptor_pool(self.descriptor_pool, None);
            }
            if self.descriptor_set_layout != vk::DescriptorSetLayout::null() {
                self.runtime
                    .device
                    .destroy_descriptor_set_layout(self.descriptor_set_layout, None);
            }
            if self.surface != vk::SurfaceKHR::null() {
                self.runtime
                    .surface_loader
                    .destroy_surface(self.surface, None);
            }
        }
    }
}

fn full_color_range() -> vk::ImageSubresourceRange {
    vk::ImageSubresourceRange::default()
        .aspect_mask(vk::ImageAspectFlags::COLOR)
        .base_mip_level(0)
        .level_count(1)
        .base_array_layer(0)
        .layer_count(1)
}
