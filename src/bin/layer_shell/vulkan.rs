use std::collections::{HashSet, VecDeque};
use std::ffi::{c_void, CStr, CString};
use std::sync::Arc;
use std::time::{Duration, Instant};

use anyhow::{anyhow, bail, Context, Result};
use ash::vk::{self, Handle};
use wayland_client::protocol::wl_surface::WlSurface;
use wayland_client::{Connection, Proxy};
use waywallen_display as sys;

const FRAMES_IN_FLIGHT: usize = 2;
const MAX_BLUR_MIP_LEVELS: u32 = 6;
const BLUR_WEIGHT_COUNT: usize = MAX_BLUR_MIP_LEVELS as usize;
const COMPOSITION_PUSH_CONSTANT_SIZE: u32 = std::mem::size_of::<CompositionPushConstants>() as u32;
const BLUR_PUSH_CONSTANT_SIZE: u32 = 8 * 4;
const BLUR_PUSH_CONSTANT_OFFSET: u32 = COMPOSITION_PUSH_CONSTANT_SIZE;
const TOTAL_PUSH_CONSTANT_SIZE: u32 = BLUR_PUSH_CONSTANT_OFFSET + BLUR_PUSH_CONSTANT_SIZE;
const RESOURCE_RETIRE_TIMEOUT_NS: u64 = 2_000_000_000;
const BLUR_TRANSITION_DURATION: Duration = Duration::from_millis(180);
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
    _entry: ash::Entry,
    instance: ash::Instance,
    surface_loader: ash::khr::surface::Instance,
    wayland_surface_loader: ash::khr::wayland_surface::Instance,
    device: ash::Device,
    swapchain_loader: ash::khr::swapchain::Device,
    external_semaphore_fd_loader: ash::khr::external_semaphore_fd::Device,
    physical_device: vk::PhysicalDevice,
    graphics_queue_family: u32,
    present_queue_family: u32,
    graphics_queue: vk::Queue,
    present_queue: vk::Queue,
    debug_utils: Option<ash::ext::debug_utils::Instance>,
    debug_messenger: vk::DebugUtilsMessengerEXT,
}

impl VulkanRuntime {
    pub fn new(
        conn: &Connection,
        surfaces: &[(u32, WlSurface)],
        compositor_drm: (u32, u32),
    ) -> Result<(Arc<Self>, Vec<(u32, vk::SurfaceKHR)>)> {
        if surfaces.is_empty() {
            bail!("cannot initialize Vulkan without a Wayland surface");
        }
        let requirements = importer_requirements()?;
        let entry = unsafe { ash::Entry::load() }.context("load Vulkan loader")?;
        let app_name = CString::new("waywallen-layer-shell").unwrap();
        let app_info = vk::ApplicationInfo::default()
            .application_name(&app_name)
            .application_version(1)
            .engine_name(&app_name)
            .engine_version(1)
            .api_version(requirements.api_version);
        let validation_layer = CString::new("VK_LAYER_KHRONOS_validation").unwrap();
        let validation_available = cfg!(debug_assertions)
            && unsafe { entry.enumerate_instance_layer_properties() }
                .context("vkEnumerateInstanceLayerProperties")?
                .iter()
                .any(|layer| {
                    (unsafe { CStr::from_ptr(layer.layer_name.as_ptr()) })
                        == validation_layer.as_c_str()
                });
        let debug_utils_available = cfg!(debug_assertions)
            && unsafe { entry.enumerate_instance_extension_properties(None) }
                .context("vkEnumerateInstanceExtensionProperties")?
                .iter()
                .any(|extension| {
                    (unsafe { CStr::from_ptr(extension.extension_name.as_ptr()) })
                        == ash::ext::debug_utils::NAME
                });
        let debug_enabled = validation_available && debug_utils_available;
        let mut instance_extensions = vec![
            ash::khr::surface::NAME.as_ptr(),
            ash::khr::wayland_surface::NAME.as_ptr(),
        ];
        if debug_enabled {
            instance_extensions.push(ash::ext::debug_utils::NAME.as_ptr());
        }
        let validation_layers = validation_available
            .then_some(validation_layer.as_ptr())
            .into_iter()
            .collect::<Vec<_>>();
        let instance_info = vk::InstanceCreateInfo::default()
            .application_info(&app_info)
            .enabled_extension_names(&instance_extensions)
            .enabled_layer_names(&validation_layers);
        let instance =
            unsafe { entry.create_instance(&instance_info, None) }.context("vkCreateInstance")?;
        let surface_loader = ash::khr::surface::Instance::new(&entry, &instance);
        let wayland_surface_loader = ash::khr::wayland_surface::Instance::new(&entry, &instance);

        let mut vk_surfaces = Vec::with_capacity(surfaces.len());
        for (output, surface) in surfaces {
            match create_wayland_surface(conn, surface, &wayland_surface_loader) {
                Ok(vk_surface) => vk_surfaces.push((*output, vk_surface)),
                Err(error) => {
                    for (_, vk_surface) in vk_surfaces.drain(..) {
                        unsafe { surface_loader.destroy_surface(vk_surface, None) };
                    }
                    unsafe { instance.destroy_instance(None) };
                    return Err(error);
                }
            }
        }

        let selected = select_physical_device(
            &instance,
            &surface_loader,
            &wayland_surface_loader,
            conn.backend().display_ptr().cast(),
            &vk_surfaces,
            &requirements.device_extensions,
            compositor_drm,
        );
        let (physical_device, graphics_queue_family, present_queue_family, device_name) =
            match selected {
                Ok(selected) => selected,
                Err(error) => {
                    for (_, vk_surface) in vk_surfaces.drain(..) {
                        unsafe { surface_loader.destroy_surface(vk_surface, None) };
                    }
                    unsafe { instance.destroy_instance(None) };
                    return Err(error);
                }
            };

        let priorities = [1.0f32];
        let mut queue_families = vec![graphics_queue_family];
        if present_queue_family != graphics_queue_family {
            queue_families.push(present_queue_family);
        }
        let queue_infos: Vec<_> = queue_families
            .iter()
            .map(|family| {
                vk::DeviceQueueCreateInfo::default()
                    .queue_family_index(*family)
                    .queue_priorities(&priorities)
            })
            .collect();
        let mut device_extensions: Vec<*const i8> = requirements
            .device_extensions
            .iter()
            .map(|extension| extension.as_ptr())
            .collect();
        device_extensions.push(ash::khr::swapchain::NAME.as_ptr());
        let device_info = vk::DeviceCreateInfo::default()
            .queue_create_infos(&queue_infos)
            .enabled_extension_names(&device_extensions);
        let device = match unsafe { instance.create_device(physical_device, &device_info, None) } {
            Ok(device) => device,
            Err(error) => {
                for (_, vk_surface) in vk_surfaces.drain(..) {
                    unsafe { surface_loader.destroy_surface(vk_surface, None) };
                }
                unsafe { instance.destroy_instance(None) };
                return Err(anyhow!("vkCreateDevice: {error:?}"));
            }
        };
        let graphics_queue = unsafe { device.get_device_queue(graphics_queue_family, 0) };
        let present_queue = unsafe { device.get_device_queue(present_queue_family, 0) };
        let swapchain_loader = ash::khr::swapchain::Device::new(&instance, &device);
        let external_semaphore_fd_loader =
            ash::khr::external_semaphore_fd::Device::new(&instance, &device);
        let (debug_utils, debug_messenger) = if debug_enabled {
            let loader = ash::ext::debug_utils::Instance::new(&entry, &instance);
            let info = vk::DebugUtilsMessengerCreateInfoEXT::default()
                .message_severity(
                    vk::DebugUtilsMessageSeverityFlagsEXT::WARNING
                        | vk::DebugUtilsMessageSeverityFlagsEXT::ERROR,
                )
                .message_type(
                    vk::DebugUtilsMessageTypeFlagsEXT::GENERAL
                        | vk::DebugUtilsMessageTypeFlagsEXT::VALIDATION
                        | vk::DebugUtilsMessageTypeFlagsEXT::PERFORMANCE,
                )
                .pfn_user_callback(Some(vulkan_debug_callback));
            match unsafe { loader.create_debug_utils_messenger(&info, None) } {
                Ok(messenger) => (Some(loader), messenger),
                Err(error) => {
                    log::warn!("vkCreateDebugUtilsMessengerEXT failed: {error:?}");
                    (None, vk::DebugUtilsMessengerEXT::null())
                }
            }
        } else {
            log::debug!(
                "Vulkan validation callback disabled: layer_available={validation_available} \
                 debug_utils_available={debug_utils_available}"
            );
            (None, vk::DebugUtilsMessengerEXT::null())
        };
        log::info!(
            "Vulkan WSI runtime ready: device={device_name} graphics_qfi={graphics_queue_family} \
             present_qfi={present_queue_family} targets={}",
            vk_surfaces.len()
        );

        Ok((
            Arc::new(Self {
                _entry: entry,
                instance,
                surface_loader,
                wayland_surface_loader,
                device,
                swapchain_loader,
                external_semaphore_fd_loader,
                physical_device,
                graphics_queue_family,
                present_queue_family,
                graphics_queue,
                present_queue,
                debug_utils,
                debug_messenger,
            }),
            vk_surfaces,
        ))
    }

    pub fn create_surface(&self, conn: &Connection, surface: &WlSurface) -> Result<vk::SurfaceKHR> {
        let vk_surface = create_wayland_surface(conn, surface, &self.wayland_surface_loader)?;
        let supported = unsafe {
            self.surface_loader.get_physical_device_surface_support(
                self.physical_device,
                self.present_queue_family,
                vk_surface,
            )
        }
        .context("query hot-plug surface support")?;
        if !supported {
            unsafe { self.surface_loader.destroy_surface(vk_surface, None) };
            bail!("selected Vulkan device cannot present to the hot-plugged Wayland surface");
        }
        Ok(vk_surface)
    }

    pub fn destroy_surface(&self, surface: vk::SurfaceKHR) {
        unsafe { self.surface_loader.destroy_surface(surface, None) };
    }

    pub fn display_context(&self) -> sys::waywallen_vk_ctx_t {
        sys::waywallen_vk_ctx_t {
            instance: self.instance.handle().as_raw() as usize as *mut c_void,
            physical_device: self.physical_device.as_raw() as usize as *mut c_void,
            device: self.device.handle().as_raw() as usize as *mut c_void,
            queue_family_index: self.graphics_queue_family,
            vk_get_instance_proc_addr: unsafe {
                std::mem::transmute(self._entry.static_fn().get_instance_proc_addr)
            },
        }
    }
}

impl Drop for VulkanRuntime {
    fn drop(&mut self) {
        unsafe {
            let _ = self.device.device_wait_idle();
            self.device.destroy_device(None);
            if let Some(debug_utils) = self.debug_utils.as_ref() {
                debug_utils.destroy_debug_utils_messenger(self.debug_messenger, None);
            }
            self.instance.destroy_instance(None);
        }
    }
}

unsafe extern "system" fn vulkan_debug_callback(
    severity: vk::DebugUtilsMessageSeverityFlagsEXT,
    _message_types: vk::DebugUtilsMessageTypeFlagsEXT,
    callback_data: *const vk::DebugUtilsMessengerCallbackDataEXT<'_>,
    _user_data: *mut c_void,
) -> vk::Bool32 {
    let message = if callback_data.is_null() || (*callback_data).p_message.is_null() {
        "Vulkan validation message without text".into()
    } else {
        CStr::from_ptr((*callback_data).p_message).to_string_lossy()
    };
    if severity.contains(vk::DebugUtilsMessageSeverityFlagsEXT::ERROR) {
        log::error!("Vulkan validation: {message}");
    } else {
        log::warn!("Vulkan validation: {message}");
    }
    vk::FALSE
}

struct ImporterRequirements {
    api_version: u32,
    device_extensions: Vec<CString>,
}

fn importer_requirements() -> Result<ImporterRequirements> {
    let mut raw = sys::waywallen_vk_requirements_t {
        api_version: 0,
        device_extensions: std::ptr::null(),
        device_extension_count: 0,
        imported_image_usage: 0,
        imported_image_layout: 0,
        external_queue_family_index: 0,
    };
    let rc = unsafe { sys::waywallen_display_vulkan_requirements(&mut raw) };
    if rc != sys::WAYWALLEN_OK {
        bail!("query display Vulkan requirements failed: {rc}");
    }
    let extension_ptrs = unsafe {
        std::slice::from_raw_parts(raw.device_extensions, raw.device_extension_count as usize)
    };
    let device_extensions = extension_ptrs
        .iter()
        .map(|ptr| {
            if ptr.is_null() {
                bail!("display Vulkan requirements contain a null extension");
            }
            Ok(CString::new(unsafe { CStr::from_ptr(*ptr) }.to_bytes()).unwrap())
        })
        .collect::<Result<Vec<_>>>()?;
    Ok(ImporterRequirements {
        api_version: raw.api_version,
        device_extensions,
    })
}

fn create_wayland_surface(
    conn: &Connection,
    surface: &WlSurface,
    loader: &ash::khr::wayland_surface::Instance,
) -> Result<vk::SurfaceKHR> {
    let display = conn.backend().display_ptr();
    let wl_surface = surface.id().as_ptr();
    if display.is_null() || wl_surface.is_null() {
        bail!("wayland-client did not expose system display/surface handles");
    }
    let info = vk::WaylandSurfaceCreateInfoKHR::default()
        .display(display.cast())
        .surface(wl_surface.cast());
    unsafe { loader.create_wayland_surface(&info, None) }.context("vkCreateWaylandSurfaceKHR")
}

fn select_physical_device(
    instance: &ash::Instance,
    surface_loader: &ash::khr::surface::Instance,
    wayland_surface_loader: &ash::khr::wayland_surface::Instance,
    wayland_display: *mut vk::wl_display,
    surfaces: &[(u32, vk::SurfaceKHR)],
    importer_extensions: &[CString],
    compositor_drm: (u32, u32),
) -> Result<(vk::PhysicalDevice, u32, u32, String)> {
    let devices =
        unsafe { instance.enumerate_physical_devices() }.context("vkEnumeratePhysicalDevices")?;
    let mut candidates = Vec::new();
    for physical_device in devices {
        let extensions = unsafe { instance.enumerate_device_extension_properties(physical_device) }
            .context("vkEnumerateDeviceExtensionProperties")?;
        let available: HashSet<Vec<u8>> = extensions
            .iter()
            .map(|extension| {
                unsafe { CStr::from_ptr(extension.extension_name.as_ptr()) }
                    .to_bytes()
                    .to_vec()
            })
            .collect();
        let importer_ok = importer_extensions
            .iter()
            .all(|extension| available.contains(extension.as_bytes()));
        if !importer_ok || !available.contains(ash::khr::swapchain::NAME.to_bytes()) {
            continue;
        }
        let queue_properties =
            unsafe { instance.get_physical_device_queue_family_properties(physical_device) };
        let graphics_families = queue_properties
            .iter()
            .enumerate()
            .filter(|(_, properties)| {
                properties.queue_count > 0
                    && properties.queue_flags.contains(vk::QueueFlags::GRAPHICS)
            })
            .map(|(index, _)| index as u32)
            .collect::<Vec<_>>();
        if graphics_families.is_empty() {
            continue;
        }
        let mut present_families = Vec::new();
        for family in 0..queue_properties.len() as u32 {
            if queue_properties[family as usize].queue_count == 0 {
                continue;
            }
            if !unsafe {
                wayland_surface_loader.get_physical_device_wayland_presentation_support(
                    physical_device,
                    family,
                    &mut *wayland_display,
                )
            } {
                continue;
            }
            let mut all_supported = true;
            for (_, surface) in surfaces {
                let supported = unsafe {
                    surface_loader.get_physical_device_surface_support(
                        physical_device,
                        family,
                        *surface,
                    )
                }
                .context("vkGetPhysicalDeviceSurfaceSupportKHR")?;
                if !supported {
                    all_supported = false;
                    break;
                }
            }
            if all_supported {
                present_families.push(family);
            }
        }
        let Some((graphics_queue_family, present_queue_family)) =
            choose_queue_families(&graphics_families, &present_families)
        else {
            continue;
        };

        let properties = unsafe { instance.get_physical_device_properties(physical_device) };
        let name = unsafe { CStr::from_ptr(properties.device_name.as_ptr()) }
            .to_string_lossy()
            .into_owned();
        let mut drm = vk::PhysicalDeviceDrmPropertiesEXT::default();
        let mut properties2 = vk::PhysicalDeviceProperties2::default().push_next(&mut drm);
        unsafe { instance.get_physical_device_properties2(physical_device, &mut properties2) };
        let drm_match = compositor_drm != (0, 0)
            && drm.has_render != 0
            && drm.render_major == compositor_drm.0 as i64
            && drm.render_minor == compositor_drm.1 as i64;
        let unified_queue = graphics_queue_family == present_queue_family;
        candidates.push((
            drm_match,
            unified_queue,
            physical_device,
            graphics_queue_family,
            present_queue_family,
            name,
        ));
    }
    candidates.sort_by_key(|candidate| device_candidate_sort_key(candidate.0, candidate.1));
    candidates
        .into_iter()
        .next()
        .map(|(_, _, physical_device, graphics, present, name)| {
            (physical_device, graphics, present, name)
        })
        .ok_or_else(|| {
            anyhow!(
                "no Vulkan device satisfies display DMA-BUF import requirements and all Wayland targets"
            )
        })
}

fn device_candidate_sort_key(drm_match: bool, unified_queue: bool) -> (bool, bool) {
    (!drm_match, !unified_queue)
}

fn choose_queue_families(graphics: &[u32], present: &[u32]) -> Option<(u32, u32)> {
    let fallback_present = *present.first()?;
    graphics
        .iter()
        .copied()
        .find(|family| present.contains(family))
        .map(|family| (family, family))
        .or_else(|| {
            graphics
                .first()
                .copied()
                .map(|family| (family, fallback_present))
        })
}

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

impl PausePresentation {
    fn target_radius(self) -> f32 {
        if self.configured && self.active {
            self.radius as f32
        } else {
            0.0
        }
    }
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

fn needs_persistent_scene(
    available: bool,
    presentation: PausePresentation,
    blur: &BlurSample,
    scene_exists: bool,
) -> bool {
    available
        && (presentation.configured
            || blur.radius > f32::EPSILON
            || blur.animating
            || (blur.finished && scene_exists))
}

impl BlurTransition {
    fn set_target(&mut self, target: f32, now: Instant, animate: bool) -> bool {
        let current = self.value_at(now);
        self.current = current;
        if (target - self.target).abs() <= f32::EPSILON {
            return false;
        }
        self.target = target;
        if !animate || (target - current).abs() <= f32::EPSILON {
            self.current = target;
            self.segment = None;
        } else {
            self.segment = Some(BlurSegment {
                from: current,
                target,
                started_at: now,
            });
        }
        true
    }

    fn reset(&mut self) {
        *self = Self::default();
    }

    fn sample(&mut self, now: Instant) -> BlurSample {
        let Some(segment) = self.segment else {
            return BlurSample {
                radius: self.current,
                animating: false,
                finished: false,
            };
        };
        let elapsed = now.saturating_duration_since(segment.started_at);
        if elapsed >= BLUR_TRANSITION_DURATION {
            self.current = segment.target;
            self.segment = None;
            return BlurSample {
                radius: self.current,
                animating: false,
                finished: true,
            };
        }
        self.current = interpolate_blur(segment, elapsed);
        BlurSample {
            radius: self.current,
            animating: true,
            finished: false,
        }
    }

    fn value_at(&self, now: Instant) -> f32 {
        let Some(segment) = self.segment else {
            return self.current;
        };
        let elapsed = now.saturating_duration_since(segment.started_at);
        if elapsed >= BLUR_TRANSITION_DURATION {
            segment.target
        } else {
            interpolate_blur(segment, elapsed)
        }
    }
}

fn interpolate_blur(segment: BlurSegment, elapsed: Duration) -> f32 {
    let progress = elapsed.as_secs_f32() / BLUR_TRANSITION_DURATION.as_secs_f32();
    let inverse = 1.0 - progress.clamp(0.0, 1.0);
    let eased = 1.0 - inverse * inverse * inverse;
    segment.from + (segment.target - segment.from) * eased
}

struct BlurResources {
    image: vk::Image,
    memory: vk::DeviceMemory,
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

struct DirectBinding {
    generation: u64,
    images: Vec<vk::Image>,
    views: Vec<vk::ImageView>,
    format: vk::Format,
    extent: vk::Extent2D,
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
    release_finished: vk::Semaphore,
    fence: vk::Fence,
    descriptor_set: vk::DescriptorSet,
    cpu_release_fallback: Option<DirectRelease>,
    recreate_release_semaphore: bool,
}

struct RetiredSwapchain {
    handle: vk::SwapchainKHR,
    present_ready: Vec<vk::Semaphore>,
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
    direct_binding: Option<DirectBinding>,
    direct_frames: VecDeque<DirectFrame>,
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
            direct_binding: None,
            direct_frames: VecDeque::new(),
            recreate_extent: None,
            retired_swapchains: Vec::new(),
        };
        presenter.initialize(extent)?;
        Ok(presenter)
    }

    fn initialize(&mut self, extent: (u32, u32)) -> Result<()> {
        let descriptor_binding = [
            vk::DescriptorSetLayoutBinding::default()
                .binding(0)
                .descriptor_type(vk::DescriptorType::COMBINED_IMAGE_SAMPLER)
                .descriptor_count(1)
                .stage_flags(vk::ShaderStageFlags::FRAGMENT),
            vk::DescriptorSetLayoutBinding::default()
                .binding(1)
                .descriptor_type(vk::DescriptorType::COMBINED_IMAGE_SAMPLER)
                .descriptor_count(1)
                .stage_flags(vk::ShaderStageFlags::FRAGMENT),
        ];
        let descriptor_layout_info =
            vk::DescriptorSetLayoutCreateInfo::default().bindings(&descriptor_binding);
        self.descriptor_set_layout = unsafe {
            self.runtime
                .device
                .create_descriptor_set_layout(&descriptor_layout_info, None)
        }
        .context("vkCreateDescriptorSetLayout")?;
        let pool_sizes = [vk::DescriptorPoolSize {
            ty: vk::DescriptorType::COMBINED_IMAGE_SAMPLER,
            descriptor_count: ((FRAMES_IN_FLIGHT + MAX_BLUR_MIP_LEVELS as usize) * 2) as u32,
        }];
        let pool_info = vk::DescriptorPoolCreateInfo::default()
            .flags(vk::DescriptorPoolCreateFlags::FREE_DESCRIPTOR_SET)
            .max_sets(FRAMES_IN_FLIGHT as u32 + MAX_BLUR_MIP_LEVELS)
            .pool_sizes(&pool_sizes);
        self.descriptor_pool =
            unsafe { self.runtime.device.create_descriptor_pool(&pool_info, None) }
                .context("vkCreateDescriptorPool")?;
        let set_layouts = vec![self.descriptor_set_layout; FRAMES_IN_FLIGHT];
        let alloc_info = vk::DescriptorSetAllocateInfo::default()
            .descriptor_pool(self.descriptor_pool)
            .set_layouts(&set_layouts);
        let descriptor_sets = unsafe { self.runtime.device.allocate_descriptor_sets(&alloc_info) }
            .context("vkAllocateDescriptorSets")?;
        let push_range = [
            vk::PushConstantRange::default()
                .stage_flags(vk::ShaderStageFlags::VERTEX)
                .offset(0)
                .size(COMPOSITION_PUSH_CONSTANT_SIZE),
            vk::PushConstantRange::default()
                .stage_flags(vk::ShaderStageFlags::FRAGMENT)
                .offset(BLUR_PUSH_CONSTANT_OFFSET)
                .size(BLUR_PUSH_CONSTANT_SIZE),
        ];
        let layouts = [self.descriptor_set_layout];
        let pipeline_layout_info = vk::PipelineLayoutCreateInfo::default()
            .set_layouts(&layouts)
            .push_constant_ranges(&push_range);
        self.pipeline_layout = unsafe {
            self.runtime
                .device
                .create_pipeline_layout(&pipeline_layout_info, None)
        }
        .context("vkCreatePipelineLayout")?;
        let sampler_info = vk::SamplerCreateInfo::default()
            .mag_filter(vk::Filter::LINEAR)
            .min_filter(vk::Filter::LINEAR)
            .mipmap_mode(vk::SamplerMipmapMode::NEAREST)
            .address_mode_u(vk::SamplerAddressMode::CLAMP_TO_EDGE)
            .address_mode_v(vk::SamplerAddressMode::CLAMP_TO_EDGE)
            .address_mode_w(vk::SamplerAddressMode::CLAMP_TO_EDGE)
            .max_lod((MAX_BLUR_MIP_LEVELS - 1) as f32);
        self.sampler = unsafe { self.runtime.device.create_sampler(&sampler_info, None) }
            .context("vkCreateSampler")?;

        let properties = unsafe {
            self.runtime
                .instance
                .get_physical_device_properties(self.runtime.physical_device)
        };
        if properties.limits.max_push_constants_size < TOTAL_PUSH_CONSTANT_SIZE {
            bail!(
                "maxPushConstantsSize={} is below renderer requirement {}",
                properties.limits.max_push_constants_size,
                TOTAL_PUSH_CONSTANT_SIZE
            );
        }

        for descriptor_set in descriptor_sets {
            self.frames.push(FrameContext {
                command_pool: vk::CommandPool::null(),
                command_buffer: vk::CommandBuffer::null(),
                image_available: vk::Semaphore::null(),
                release_finished: vk::Semaphore::null(),
                fence: vk::Fence::null(),
                descriptor_set,
                cpu_release_fallback: None,
                recreate_release_semaphore: false,
            });
            let frame = self.frames.last_mut().unwrap();
            let pool_info = vk::CommandPoolCreateInfo::default()
                .flags(vk::CommandPoolCreateFlags::RESET_COMMAND_BUFFER)
                .queue_family_index(self.runtime.graphics_queue_family);
            frame.command_pool =
                unsafe { self.runtime.device.create_command_pool(&pool_info, None) }
                    .context("vkCreateCommandPool")?;
            let command_info = vk::CommandBufferAllocateInfo::default()
                .command_pool(frame.command_pool)
                .level(vk::CommandBufferLevel::PRIMARY)
                .command_buffer_count(1);
            frame.command_buffer =
                unsafe { self.runtime.device.allocate_command_buffers(&command_info) }
                    .context("vkAllocateCommandBuffers")?[0];
            let semaphore_info = vk::SemaphoreCreateInfo::default();
            frame.image_available =
                unsafe { self.runtime.device.create_semaphore(&semaphore_info, None) }
                    .context("create image-available semaphore")?;
            frame.release_finished = create_release_semaphore(&self.runtime.device)?;
            let fence_info = vk::FenceCreateInfo::default().flags(vk::FenceCreateFlags::SIGNALED);
            frame.fence = unsafe { self.runtime.device.create_fence(&fence_info, None) }
                .context("vkCreateFence")?;
        }
        self.recreate_swapchain(vk::Extent2D {
            width: extent.0,
            height: extent.1,
        })?;
        Ok(())
    }

    pub fn request_resize(&mut self, width: u32, height: u32) {
        let extent = vk::Extent2D { width, height };
        if self.extent != extent {
            self.recreate_extent = Some(extent);
        }
    }

    pub fn supports_pause_blur(&self) -> bool {
        self.pause_blur_available
    }

    pub fn has_direct_binding(&self) -> bool {
        self.direct_binding.is_some()
    }

    pub fn install_direct_binding(
        &mut self,
        generation: u64,
        extent: vk::Extent2D,
        images: &[vk::Image],
    ) -> Result<()> {
        if self.direct_binding.is_some() {
            bail!("replace direct binding before retiring its image views");
        }
        self.direct_binding = Some(DirectBinding {
            generation,
            images: images.to_vec(),
            views: vec![vk::ImageView::null(); images.len()],
            format: vk::Format::UNDEFINED,
            extent,
        });
        Ok(())
    }

    pub fn enqueue_direct_frame(
        &mut self,
        frame: &sys::waywallen_frame_t,
        direct: &sys::waywallen_vk_direct_frame_t,
    ) -> Result<()> {
        let binding = self
            .direct_binding
            .as_mut()
            .ok_or_else(|| anyhow!("direct frame arrived without an imported binding"))?;
        let index = usize::try_from(frame.buffer_index).context("direct frame buffer index")?;
        let format = vk::Format::from_raw(direct.format as i32);
        if frame.buffer_generation != binding.generation
            || index >= binding.images.len()
            || binding.images[index].as_raw() != direct.image as usize as u64
            || binding.extent.width != direct.width
            || binding.extent.height != direct.height
        {
            bail!("direct frame does not match the active imported binding");
        }
        if frame.vk_acquire_semaphore.is_null() || frame.release_syncobj_fd < 0 {
            bail!("direct frame is missing acquire or release synchronization");
        }
        if self.direct_frames.len() >= binding.images.len() {
            bail!("direct frame queue exhausted the imported buffer pool");
        }
        if binding.format == vk::Format::UNDEFINED {
            binding.format = format;
        } else if binding.format != format {
            bail!("direct frame format changed within one imported binding");
        }
        if binding.views[index] == vk::ImageView::null() {
            let info = vk::ImageViewCreateInfo::default()
                .image(binding.images[index])
                .view_type(vk::ImageViewType::TYPE_2D)
                .format(format)
                .subresource_range(full_color_range());
            binding.views[index] = unsafe { self.runtime.device.create_image_view(&info, None) }
                .context("create direct imported image view")?;
        }
        self.direct_frames.push_back(DirectFrame {
            image: binding.images[index],
            view: binding.views[index],
            extent: binding.extent,
            layout: vk::ImageLayout::from_raw(direct.layout as i32),
            external_queue_family: direct.external_queue_family_index,
            acquire_semaphore: vk::Semaphore::from_raw(frame.vk_acquire_semaphore as usize as u64),
            release_syncobj_fd: frame.release_syncobj_fd,
            buffer_generation: frame.buffer_generation,
            seq: frame.seq,
        });
        Ok(())
    }

    pub fn discard_direct_frames(&mut self, display: *mut sys::waywallen_display_t) -> Result<()> {
        let mut first_error = None;
        while let Some(frame) = self.direct_frames.pop_front() {
            if let Err(error) = resolve_direct_release(
                display,
                DirectRelease {
                    release_syncobj_fd: frame.release_syncobj_fd,
                    buffer_generation: frame.buffer_generation,
                    seq: frame.seq,
                },
            ) {
                if first_error.is_none() {
                    first_error = Some(error);
                }
            }
        }
        if let Some(error) = first_error {
            return Err(error);
        }
        Ok(())
    }

    pub fn retire_direct_binding(&mut self, display: *mut sys::waywallen_display_t) -> Result<()> {
        let discard_result = self.discard_direct_frames(display);
        if let Err(error) = self.wait_frames_idle() {
            log::warn!("timed Vulkan retirement failed, waiting for the shared device: {error:#}");
            unsafe { self.runtime.device.device_wait_idle() }
                .context("wait for shared Vulkan device before retiring direct binding")?;
        }
        let release_result = self.drain_cpu_release_fallbacks(display);
        if let Some(binding) = self.direct_binding.take() {
            unsafe {
                for view in binding.views {
                    if view != vk::ImageView::null() {
                        self.runtime.device.destroy_image_view(view, None);
                    }
                }
            }
        }
        discard_result?;
        release_result?;
        Ok(())
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
        if let Some(resources) = self.blur_resources.as_mut() {
            resources.base_valid = false;
            resources.pyramid_valid = false;
            resources.pyramid_dirty = false;
        }
    }

    pub fn present(
        &mut self,
        display: *mut sys::waywallen_display_t,
        composition: &Composition,
        now: Instant,
    ) -> Result<PresentResult> {
        let mut release_pending = self.drain_cpu_release_fallbacks(display)?;
        let blur = self.blur_transition.sample(now);
        let scene_path = needs_persistent_scene(
            self.pause_blur_available,
            self.pause_presentation,
            &blur,
            self.blur_resources.is_some(),
        );
        if !scene_path && self.blur_resources.is_some() {
            if !self.frames_idle()? {
                return Ok(PresentResult::Pending);
            }
            self.destroy_current_blur_resources();
        }
        let pending_direct = self.direct_frames.front().map(direct_frame_handles);
        if let Some(extent) = self.recreate_extent {
            if !self.frames_idle()? {
                return Ok(PresentResult::Pending);
            }
            self.recreate_extent = None;
            self.recreate_swapchain(extent)?;
        }

        if scene_path && pending_direct.is_some() {
            let extent_mismatch = self
                .blur_resources
                .as_ref()
                .is_some_and(|resources| resources.extent != self.extent);
            if extent_mismatch {
                if !self.frames_idle()? {
                    return Ok(PresentResult::Pending);
                }
                self.destroy_current_blur_resources();
            }
        }
        if scene_path && pending_direct.is_some() && self.blur_resources.is_none() {
            match self.create_blur_resources(self.render_pass, self.format, self.extent) {
                Ok(resources) => self.blur_resources = Some(resources),
                Err(error) => {
                    self.pause_blur_available = false;
                    return Err(error.context(format!(
                        "initialize Pause Blur for {}x{} {:?}",
                        self.extent.width, self.extent.height, self.format
                    )));
                }
            }
        }
        let scene_valid = self
            .blur_resources
            .as_ref()
            .is_some_and(|resources| resources.base_valid);
        if pending_direct.is_none() && !(scene_path && scene_valid) {
            return Ok(PresentResult::Presented {
                redraw: release_pending,
            });
        }

        let frame_index = self.frame_cursor;
        let frame = &self.frames[frame_index];
        if !unsafe { self.runtime.device.get_fence_status(frame.fence) }
            .context("query WSI frame fence")?
        {
            return Ok(PresentResult::Pending);
        }
        let acquired = unsafe {
            self.runtime.swapchain_loader.acquire_next_image(
                self.swapchain,
                0,
                frame.image_available,
                vk::Fence::null(),
            )
        };
        let (image_index, suboptimal) = match acquired {
            Ok(result) => result,
            Err(vk::Result::ERROR_OUT_OF_DATE_KHR) => {
                self.recreate_extent = Some(self.extent);
                return Ok(PresentResult::Pending);
            }
            Err(vk::Result::TIMEOUT) | Err(vk::Result::NOT_READY) => {
                return Ok(PresentResult::Pending)
            }
            Err(error) => return Err(anyhow!("vkAcquireNextImageKHR: {error:?}")),
        };
        unsafe { self.runtime.device.reset_fences(&[frame.fence]) }
            .context("reset WSI frame fence")?;
        unsafe {
            self.runtime
                .device
                .reset_command_pool(frame.command_pool, vk::CommandPoolResetFlags::empty())
        }
        .context("reset WSI command pool")?;

        let source_info = pending_direct.map(|(_, view, _, _, _, _)| {
            [vk::DescriptorImageInfo::default()
                .sampler(self.sampler)
                .image_view(view)
                .image_layout(vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL)]
        });
        if let Some(source_info) = source_info.as_ref() {
            let writes = [vk::WriteDescriptorSet::default()
                .dst_set(frame.descriptor_set)
                .dst_binding(0)
                .descriptor_type(vk::DescriptorType::COMBINED_IMAGE_SAMPLER)
                .image_info(source_info)];
            unsafe { self.runtime.device.update_descriptor_sets(&writes, &[]) };
        }

        let begin = vk::CommandBufferBeginInfo::default()
            .flags(vk::CommandBufferUsageFlags::ONE_TIME_SUBMIT);
        unsafe {
            self.runtime
                .device
                .begin_command_buffer(frame.command_buffer, &begin)
        }
        .context("vkBeginCommandBuffer")?;
        let clear = [vk::ClearValue {
            color: vk::ClearColorValue {
                float32: composition.clear,
            },
        }];
        let swapchain_render_area = vk::Rect2D {
            offset: vk::Offset2D { x: 0, y: 0 },
            extent: self.extent,
        };
        let composition_push = pending_direct.map(|(_, _, source_extent, _, _, _)| {
            let output_extent = if scene_path {
                self.blur_resources.as_ref().unwrap().extent
            } else {
                self.extent
            };
            composition_push_constants(composition, output_extent, source_extent)
        });
        let composition_push_bytes = composition_push.as_ref().map(|push| unsafe {
            std::slice::from_raw_parts(
                std::ptr::from_ref(push).cast::<u8>(),
                std::mem::size_of_val(push),
            )
        });
        let swapchain_begin = vk::RenderPassBeginInfo::default()
            .render_pass(self.render_pass)
            .framebuffer(self.framebuffers[image_index as usize])
            .render_area(swapchain_render_area)
            .clear_values(&clear);
        let rebuild_pyramid = scene_path
            && self.blur_resources.as_ref().is_some_and(|resources| {
                let initialize_mips = pending_direct.is_some() && !resources.base_valid;
                initialize_mips
                    || (blur.radius > f32::EPSILON
                        && (pending_direct.is_some()
                            || resources.pyramid_dirty
                            || !resources.pyramid_valid))
            });
        unsafe {
            if let Some((source_image, _, _, old_layout, external_queue_family, _)) = pending_direct
            {
                let acquire = vk::ImageMemoryBarrier::default()
                    .src_access_mask(vk::AccessFlags::empty())
                    .dst_access_mask(vk::AccessFlags::SHADER_READ)
                    .old_layout(old_layout)
                    .new_layout(vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL)
                    .src_queue_family_index(external_queue_family)
                    .dst_queue_family_index(self.runtime.graphics_queue_family)
                    .image(source_image)
                    .subresource_range(full_color_range());
                self.runtime.device.cmd_pipeline_barrier(
                    frame.command_buffer,
                    vk::PipelineStageFlags::TOP_OF_PIPE,
                    vk::PipelineStageFlags::FRAGMENT_SHADER,
                    vk::DependencyFlags::empty(),
                    &[],
                    &[],
                    &[acquire],
                );
            }
            if scene_path {
                let resources = self.blur_resources.as_ref().unwrap();
                if let Some(push_bytes) = composition_push_bytes {
                    let scene_begin = vk::RenderPassBeginInfo::default()
                        .render_pass(resources.render_pass)
                        .framebuffer(resources.framebuffers[0])
                        .render_area(vk::Rect2D {
                            offset: vk::Offset2D { x: 0, y: 0 },
                            extent: resources.extent,
                        })
                        .clear_values(&clear);
                    self.runtime.device.cmd_begin_render_pass(
                        frame.command_buffer,
                        &scene_begin,
                        vk::SubpassContents::INLINE,
                    );
                    self.runtime.device.cmd_bind_pipeline(
                        frame.command_buffer,
                        vk::PipelineBindPoint::GRAPHICS,
                        resources.composition_pipeline,
                    );
                    self.cmd_set_render_extent(frame.command_buffer, resources.extent);
                    self.runtime.device.cmd_bind_descriptor_sets(
                        frame.command_buffer,
                        vk::PipelineBindPoint::GRAPHICS,
                        self.pipeline_layout,
                        0,
                        &[frame.descriptor_set],
                        &[],
                    );
                    self.runtime.device.cmd_push_constants(
                        frame.command_buffer,
                        self.pipeline_layout,
                        vk::ShaderStageFlags::VERTEX,
                        0,
                        push_bytes,
                    );
                    self.runtime
                        .device
                        .cmd_draw(frame.command_buffer, 6, 1, 0, 0);
                    self.runtime
                        .device
                        .cmd_end_render_pass(frame.command_buffer);
                }

                if rebuild_pyramid {
                    for target_level in 1..resources.mip_views.len() {
                        let target_extent = blur_mip_extent(resources.extent, target_level as u32);
                        let downsample_begin = vk::RenderPassBeginInfo::default()
                            .render_pass(resources.render_pass)
                            .framebuffer(resources.framebuffers[target_level])
                            .render_area(vk::Rect2D {
                                offset: vk::Offset2D { x: 0, y: 0 },
                                extent: target_extent,
                            })
                            .clear_values(&clear);
                        self.runtime.device.cmd_begin_render_pass(
                            frame.command_buffer,
                            &downsample_begin,
                            vk::SubpassContents::INLINE,
                        );
                        self.runtime.device.cmd_bind_pipeline(
                            frame.command_buffer,
                            vk::PipelineBindPoint::GRAPHICS,
                            resources.downsample_pipeline,
                        );
                        self.cmd_set_render_extent(frame.command_buffer, target_extent);
                        self.runtime.device.cmd_bind_descriptor_sets(
                            frame.command_buffer,
                            vk::PipelineBindPoint::GRAPHICS,
                            self.pipeline_layout,
                            0,
                            &[resources.downsample_descriptor_sets[target_level - 1]],
                            &[],
                        );
                        let downsample_push = [
                            1.0 / target_extent.width as f32,
                            1.0 / target_extent.height as f32,
                            0.0,
                            0.0,
                            0.0,
                            0.0,
                            0.0,
                            0.0,
                        ];
                        let downsample_bytes = std::slice::from_raw_parts(
                            downsample_push.as_ptr().cast::<u8>(),
                            std::mem::size_of_val(&downsample_push),
                        );
                        self.runtime.device.cmd_push_constants(
                            frame.command_buffer,
                            self.pipeline_layout,
                            vk::ShaderStageFlags::FRAGMENT,
                            BLUR_PUSH_CONSTANT_OFFSET,
                            downsample_bytes,
                        );
                        self.runtime
                            .device
                            .cmd_draw(frame.command_buffer, 3, 1, 0, 0);
                        self.runtime
                            .device
                            .cmd_end_render_pass(frame.command_buffer);
                    }
                }

                self.runtime.device.cmd_begin_render_pass(
                    frame.command_buffer,
                    &swapchain_begin,
                    vk::SubpassContents::INLINE,
                );
                self.runtime.device.cmd_bind_pipeline(
                    frame.command_buffer,
                    vk::PipelineBindPoint::GRAPHICS,
                    resources.blur_pipeline,
                );
                self.cmd_set_render_extent(frame.command_buffer, self.extent);
                self.runtime.device.cmd_bind_descriptor_sets(
                    frame.command_buffer,
                    vk::PipelineBindPoint::GRAPHICS,
                    self.pipeline_layout,
                    0,
                    &[resources.mix_descriptor_set],
                    &[],
                );
                let blur_push = blur_weights(blur.radius, resources.mip_views.len() as u32);
                let blur_bytes = std::slice::from_raw_parts(
                    blur_push.as_ptr().cast::<u8>(),
                    std::mem::size_of_val(&blur_push),
                );
                self.runtime.device.cmd_push_constants(
                    frame.command_buffer,
                    self.pipeline_layout,
                    vk::ShaderStageFlags::FRAGMENT,
                    BLUR_PUSH_CONSTANT_OFFSET,
                    blur_bytes,
                );
                self.runtime
                    .device
                    .cmd_draw(frame.command_buffer, 3, 1, 0, 0);
                self.runtime
                    .device
                    .cmd_end_render_pass(frame.command_buffer);
            } else {
                let push_bytes = composition_push_bytes
                    .expect("direct swapchain draw must have a producer frame");
                self.runtime.device.cmd_begin_render_pass(
                    frame.command_buffer,
                    &swapchain_begin,
                    vk::SubpassContents::INLINE,
                );
                self.runtime.device.cmd_bind_pipeline(
                    frame.command_buffer,
                    vk::PipelineBindPoint::GRAPHICS,
                    self.pipeline,
                );
                self.cmd_set_render_extent(frame.command_buffer, self.extent);
                self.runtime.device.cmd_bind_descriptor_sets(
                    frame.command_buffer,
                    vk::PipelineBindPoint::GRAPHICS,
                    self.pipeline_layout,
                    0,
                    &[frame.descriptor_set],
                    &[],
                );
                self.runtime.device.cmd_push_constants(
                    frame.command_buffer,
                    self.pipeline_layout,
                    vk::ShaderStageFlags::VERTEX,
                    0,
                    push_bytes,
                );
                self.runtime
                    .device
                    .cmd_draw(frame.command_buffer, 6, 1, 0, 0);
                self.runtime
                    .device
                    .cmd_end_render_pass(frame.command_buffer);
            }
            if let Some((source_image, _, _, old_layout, external_queue_family, _)) = pending_direct
            {
                let release = vk::ImageMemoryBarrier::default()
                    .src_access_mask(vk::AccessFlags::SHADER_READ)
                    .dst_access_mask(vk::AccessFlags::empty())
                    .old_layout(vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL)
                    .new_layout(old_layout)
                    .src_queue_family_index(self.runtime.graphics_queue_family)
                    .dst_queue_family_index(external_queue_family)
                    .image(source_image)
                    .subresource_range(full_color_range());
                self.runtime.device.cmd_pipeline_barrier(
                    frame.command_buffer,
                    vk::PipelineStageFlags::FRAGMENT_SHADER,
                    vk::PipelineStageFlags::BOTTOM_OF_PIPE,
                    vk::DependencyFlags::empty(),
                    &[],
                    &[],
                    &[release],
                );
            }
            self.runtime.device.end_command_buffer(frame.command_buffer)
        }
        .context("record WSI command buffer")?;

        let mut wait_semaphores = vec![frame.image_available];
        let mut wait_stages = vec![vk::PipelineStageFlags::COLOR_ATTACHMENT_OUTPUT];
        if let Some((_, _, _, _, _, acquire_semaphore)) = pending_direct {
            wait_semaphores.push(acquire_semaphore);
            wait_stages.push(vk::PipelineStageFlags::FRAGMENT_SHADER);
        }
        let command_buffers = [frame.command_buffer];
        let present_ready = self.present_ready[image_index as usize];
        let mut signal_semaphores = vec![present_ready];
        if pending_direct.is_some() {
            signal_semaphores.push(frame.release_finished);
        }
        let submit = [vk::SubmitInfo::default()
            .wait_semaphores(&wait_semaphores)
            .wait_dst_stage_mask(&wait_stages)
            .command_buffers(&command_buffers)
            .signal_semaphores(&signal_semaphores)];
        unsafe {
            self.runtime
                .device
                .queue_submit(self.runtime.graphics_queue, &submit, frame.fence)
        }
        .context("vkQueueSubmit WSI frame")?;
        if scene_path {
            let resources = self.blur_resources.as_mut().unwrap();
            if pending_direct.is_some() {
                resources.base_valid = true;
                resources.pyramid_valid = false;
                resources.pyramid_dirty = true;
            }
            if rebuild_pyramid {
                resources.pyramid_valid = true;
                resources.pyramid_dirty = false;
            }
        }
        let release_arm = if pending_direct.is_some() {
            let direct = self
                .direct_frames
                .pop_front()
                .expect("submitted direct source must remain queued");
            Some(self.arm_direct_release(
                display,
                frame_index,
                DirectRelease {
                    release_syncobj_fd: direct.release_syncobj_fd,
                    buffer_generation: direct.buffer_generation,
                    seq: direct.seq,
                },
            ))
        } else {
            None
        };
        let swapchains = [self.swapchain];
        let image_indices = [image_index];
        let present_wait_semaphores = [present_ready];
        let present_info = vk::PresentInfoKHR::default()
            .wait_semaphores(&present_wait_semaphores)
            .swapchains(&swapchains)
            .image_indices(&image_indices);
        let present = unsafe {
            self.runtime
                .swapchain_loader
                .queue_present(self.runtime.present_queue, &present_info)
        };
        match present {
            Ok(present_suboptimal) => {
                if suboptimal || present_suboptimal {
                    self.recreate_extent = Some(self.extent);
                }
            }
            Err(vk::Result::ERROR_OUT_OF_DATE_KHR) => {
                self.recreate_extent = Some(self.extent);
            }
            Err(error) => return Err(anyhow!("vkQueuePresentKHR: {error:?}")),
        }
        if let Some(result) = release_arm {
            release_pending |= result?;
        }
        self.frame_cursor = (self.frame_cursor + 1) % self.frames.len();
        let cleanup_pending =
            !self.pause_presentation.configured && blur.finished && self.blur_resources.is_some();
        Ok(PresentResult::Presented {
            redraw: blur.animating
                || cleanup_pending
                || release_pending
                || !self.direct_frames.is_empty(),
        })
    }

    fn recreate_swapchain(&mut self, requested: vk::Extent2D) -> Result<()> {
        if requested.width == 0 || requested.height == 0 {
            return Ok(());
        }
        let capabilities = unsafe {
            self.runtime
                .surface_loader
                .get_physical_device_surface_capabilities(
                    self.runtime.physical_device,
                    self.surface,
                )
        }
        .context("vkGetPhysicalDeviceSurfaceCapabilitiesKHR")?;
        let formats = unsafe {
            self.runtime
                .surface_loader
                .get_physical_device_surface_formats(self.runtime.physical_device, self.surface)
        }
        .context("vkGetPhysicalDeviceSurfaceFormatsKHR")?;
        if formats.is_empty() {
            bail!("Wayland surface has no Vulkan formats");
        }
        let surface_format = choose_surface_format(&formats);
        let extent = choose_extent(&capabilities, requested);
        let image_count = choose_image_count(&capabilities);
        let queue_families = [
            self.runtime.graphics_queue_family,
            self.runtime.present_queue_family,
        ];
        let mut info = vk::SwapchainCreateInfoKHR::default()
            .surface(self.surface)
            .min_image_count(image_count)
            .image_format(surface_format.format)
            .image_color_space(surface_format.color_space)
            .image_extent(extent)
            .image_array_layers(1)
            .image_usage(vk::ImageUsageFlags::COLOR_ATTACHMENT)
            .pre_transform(capabilities.current_transform)
            .composite_alpha(choose_composite_alpha(
                capabilities.supported_composite_alpha,
            ))
            .present_mode(vk::PresentModeKHR::FIFO)
            .clipped(true)
            .old_swapchain(self.swapchain);
        if self.runtime.graphics_queue_family != self.runtime.present_queue_family {
            info = info
                .image_sharing_mode(vk::SharingMode::CONCURRENT)
                .queue_family_indices(&queue_families);
        } else {
            info = info.image_sharing_mode(vk::SharingMode::EXCLUSIVE);
        }
        let new_swapchain = unsafe { self.runtime.swapchain_loader.create_swapchain(&info, None) }
            .context("vkCreateSwapchainKHR")?;
        let new_images = match unsafe {
            self.runtime
                .swapchain_loader
                .get_swapchain_images(new_swapchain)
        } {
            Ok(images) => images,
            Err(error) => {
                unsafe {
                    self.runtime
                        .swapchain_loader
                        .destroy_swapchain(new_swapchain, None)
                };
                return Err(anyhow!("vkGetSwapchainImagesKHR: {error:?}"));
            }
        };
        let new_present_ready = match self.create_present_semaphores(new_images.len()) {
            Ok(semaphores) => semaphores,
            Err(error) => {
                unsafe {
                    self.runtime
                        .swapchain_loader
                        .destroy_swapchain(new_swapchain, None)
                };
                return Err(error);
            }
        };
        let (new_views, new_render_pass, new_pipeline, new_framebuffers) =
            match self.create_swapchain_rendering(&new_images, surface_format.format, extent) {
                Ok(resources) => resources,
                Err(error) => {
                    self.destroy_semaphores(new_present_ready);
                    unsafe {
                        self.runtime
                            .swapchain_loader
                            .destroy_swapchain(new_swapchain, None)
                    };
                    return Err(error);
                }
            };
        let new_blur_pipeline = if self.blur_resources.is_some() {
            match self.create_pipeline(
                new_render_pass,
                FULLSCREEN_VERTEX_SHADER,
                BLUR_FRAGMENT_SHADER,
            ) {
                Ok(pipeline) => Some(pipeline),
                Err(error) => {
                    self.destroy_swapchain_rendering_parts(
                        new_views,
                        new_render_pass,
                        new_pipeline,
                        new_framebuffers,
                    );
                    self.destroy_semaphores(new_present_ready);
                    unsafe {
                        self.runtime
                            .swapchain_loader
                            .destroy_swapchain(new_swapchain, None)
                    };
                    return Err(error.context("recreate Pause Blur output pipeline"));
                }
            }
        } else {
            None
        };

        if let (Some(resources), Some(new_pipeline)) =
            (self.blur_resources.as_mut(), new_blur_pipeline)
        {
            let old_pipeline = std::mem::replace(&mut resources.blur_pipeline, new_pipeline);
            unsafe { self.runtime.device.destroy_pipeline(old_pipeline, None) };
        }
        self.destroy_swapchain_rendering();
        if self.swapchain != vk::SwapchainKHR::null() {
            self.retired_swapchains.push(RetiredSwapchain {
                handle: self.swapchain,
                present_ready: std::mem::take(&mut self.present_ready),
            });
        }
        self.swapchain = new_swapchain;
        self.images = new_images;
        self.present_ready = new_present_ready;
        self.image_views = new_views;
        self.render_pass = new_render_pass;
        self.pipeline = new_pipeline;
        self.framebuffers = new_framebuffers;
        self.format = surface_format.format;
        self.extent = extent;
        if self.choose_scene_format(self.format).is_none() {
            self.pause_blur_available = false;
            log::warn!(
                "Pause Blur disabled: no linear-filtered color-attachment format for {:?}",
                self.format
            );
        }
        log::info!(
            "WSI swapchain ready: {}x{} format={:?} requested_images={} actual_images={} frames_in_flight={}",
            extent.width,
            extent.height,
            self.format,
            image_count,
            self.images.len(),
            FRAMES_IN_FLIGHT
        );
        Ok(())
    }

    fn create_present_semaphores(&self, count: usize) -> Result<Vec<vk::Semaphore>> {
        let info = vk::SemaphoreCreateInfo::default();
        let mut semaphores = Vec::with_capacity(count);
        for _ in 0..count {
            match unsafe { self.runtime.device.create_semaphore(&info, None) } {
                Ok(semaphore) => semaphores.push(semaphore),
                Err(error) => {
                    self.destroy_semaphores(semaphores);
                    return Err(anyhow!("create swapchain present semaphore: {error:?}"));
                }
            }
        }
        Ok(semaphores)
    }

    fn destroy_semaphores(&self, semaphores: Vec<vk::Semaphore>) {
        unsafe {
            for semaphore in semaphores {
                self.runtime.device.destroy_semaphore(semaphore, None);
            }
        }
    }

    pub fn frames_idle(&self) -> Result<bool> {
        for frame in &self.frames {
            if !unsafe { self.runtime.device.get_fence_status(frame.fence) }
                .context("query WSI frame fence")?
            {
                return Ok(false);
            }
        }
        Ok(true)
    }

    fn drain_cpu_release_fallbacks(
        &mut self,
        display: *mut sys::waywallen_display_t,
    ) -> Result<bool> {
        let mut first_error = None;
        for frame in &mut self.frames {
            if frame.cpu_release_fallback.is_none() {
                continue;
            }
            let ready = unsafe { self.runtime.device.get_fence_status(frame.fence) }
                .context("query direct frame release fence")?;
            if ready {
                let release = frame.cpu_release_fallback.take().unwrap();
                if let Err(error) = resolve_direct_release(display, release) {
                    if first_error.is_none() {
                        first_error = Some(error);
                    }
                }
                if frame.recreate_release_semaphore {
                    unsafe {
                        self.runtime
                            .device
                            .destroy_semaphore(frame.release_finished, None)
                    };
                    match create_release_semaphore(&self.runtime.device) {
                        Ok(semaphore) => {
                            frame.release_finished = semaphore;
                            frame.recreate_release_semaphore = false;
                        }
                        Err(error) => {
                            frame.release_finished = vk::Semaphore::null();
                            if first_error.is_none() {
                                first_error = Some(error);
                            }
                        }
                    }
                }
            }
        }
        if let Some(error) = first_error {
            return Err(error);
        }
        Ok(self
            .frames
            .iter()
            .any(|frame| frame.cpu_release_fallback.is_some()))
    }

    fn arm_direct_release(
        &mut self,
        display: *mut sys::waywallen_display_t,
        frame_index: usize,
        release: DirectRelease,
    ) -> Result<bool> {
        let semaphore = self.frames[frame_index].release_finished;
        let get_info = vk::SemaphoreGetFdInfoKHR::default()
            .semaphore(semaphore)
            .handle_type(vk::ExternalSemaphoreHandleTypeFlags::SYNC_FD);
        let sync_file_fd = match unsafe {
            self.runtime
                .external_semaphore_fd_loader
                .get_semaphore_fd(&get_info)
        } {
            Ok(fd) if fd >= 0 => fd,
            Ok(_) => {
                self.frames[frame_index].cpu_release_fallback = Some(release);
                return Ok(true);
            }
            Err(error) => {
                log::warn!(
                    "export direct draw completion sync_file failed: {error:?}; using WSI fence"
                );
                self.frames[frame_index].cpu_release_fallback = Some(release);
                self.frames[frame_index].recreate_release_semaphore = true;
                return Ok(true);
            }
        };
        let fallback_fd = unsafe { libc::dup(release.release_syncobj_fd) };
        if fallback_fd < 0 {
            let error = std::io::Error::last_os_error();
            log::warn!("duplicate release syncobj failed: {error}; using WSI fence");
            unsafe { libc::close(sync_file_fd) };
            self.frames[frame_index].cpu_release_fallback = Some(release);
            return Ok(true);
        }
        let rc = unsafe {
            sys::waywallen_display_release_after_sync_file(release.release_syncobj_fd, sync_file_fd)
        };
        if rc != sys::WAYWALLEN_OK {
            log::warn!(
                "attach direct draw sync_file to release syncobj failed: {rc}; using WSI fence"
            );
            self.frames[frame_index].cpu_release_fallback = Some(DirectRelease {
                release_syncobj_fd: fallback_fd,
                ..release
            });
            return Ok(true);
        }
        unsafe { libc::close(fallback_fd) };
        let rc = unsafe {
            sys::waywallen_display_frame_release_armed(
                display,
                release.buffer_generation,
                release.seq,
            )
        };
        if rc != sys::WAYWALLEN_OK {
            bail!(
                "acknowledge GPU-linked direct release generation={} seq={} failed: {rc}",
                release.buffer_generation,
                release.seq
            );
        }
        Ok(false)
    }

    fn wait_frames_idle(&self) -> Result<()> {
        let fences = self
            .frames
            .iter()
            .map(|frame| frame.fence)
            .collect::<Vec<_>>();
        match unsafe {
            self.runtime
                .device
                .wait_for_fences(&fences, true, RESOURCE_RETIRE_TIMEOUT_NS)
        } {
            Ok(()) => Ok(()),
            Err(vk::Result::TIMEOUT) => bail!(
                "WSI frame retirement timed out after {} ms",
                RESOURCE_RETIRE_TIMEOUT_NS / 1_000_000
            ),
            Err(error) => Err(anyhow!("wait for WSI frame retirement: {error:?}")),
        }
    }

    fn choose_scene_format(&self, swapchain_format: vk::Format) -> Option<vk::Format> {
        [
            swapchain_format,
            vk::Format::R8G8B8A8_UNORM,
            vk::Format::B8G8R8A8_UNORM,
        ]
        .into_iter()
        .find(|format| {
            let properties = unsafe {
                self.runtime
                    .instance
                    .get_physical_device_format_properties(self.runtime.physical_device, *format)
            };
            properties.optimal_tiling_features.contains(
                vk::FormatFeatureFlags::COLOR_ATTACHMENT
                    | vk::FormatFeatureFlags::SAMPLED_IMAGE
                    | vk::FormatFeatureFlags::SAMPLED_IMAGE_FILTER_LINEAR,
            )
        })
    }

    fn find_memory_type(&self, bits: u32, required: vk::MemoryPropertyFlags) -> Option<u32> {
        let properties = unsafe {
            self.runtime
                .instance
                .get_physical_device_memory_properties(self.runtime.physical_device)
        };
        properties.memory_types[..properties.memory_type_count as usize]
            .iter()
            .enumerate()
            .find_map(|(index, memory_type)| {
                ((bits & (1 << index)) != 0 && memory_type.property_flags.contains(required))
                    .then_some(index as u32)
            })
    }

    fn create_blur_resources(
        &self,
        swapchain_render_pass: vk::RenderPass,
        swapchain_format: vk::Format,
        extent: vk::Extent2D,
    ) -> Result<BlurResources> {
        let format = self
            .choose_scene_format(swapchain_format)
            .ok_or_else(|| anyhow!("no linear-filtered color-attachment format for Pause Blur"))?;
        let mip_levels = blur_mip_level_count(extent);
        let mut allocation_size = 0;
        let mut resources = BlurResources {
            image: vk::Image::null(),
            memory: vk::DeviceMemory::null(),
            all_levels_view: vk::ImageView::null(),
            mip_views: Vec::with_capacity(mip_levels as usize),
            render_pass: vk::RenderPass::null(),
            composition_pipeline: vk::Pipeline::null(),
            downsample_pipeline: vk::Pipeline::null(),
            framebuffers: Vec::with_capacity(mip_levels as usize),
            blur_pipeline: vk::Pipeline::null(),
            downsample_descriptor_sets: Vec::with_capacity(mip_levels.saturating_sub(1) as usize),
            mix_descriptor_set: vk::DescriptorSet::null(),
            extent,
            base_valid: false,
            pyramid_valid: false,
            pyramid_dirty: false,
        };
        let create = (|| -> Result<()> {
            let image_info = vk::ImageCreateInfo::default()
                .image_type(vk::ImageType::TYPE_2D)
                .format(format)
                .extent(vk::Extent3D {
                    width: extent.width,
                    height: extent.height,
                    depth: 1,
                })
                .mip_levels(mip_levels)
                .array_layers(1)
                .samples(vk::SampleCountFlags::TYPE_1)
                .tiling(vk::ImageTiling::OPTIMAL)
                .usage(vk::ImageUsageFlags::COLOR_ATTACHMENT | vk::ImageUsageFlags::SAMPLED)
                .sharing_mode(vk::SharingMode::EXCLUSIVE)
                .initial_layout(vk::ImageLayout::UNDEFINED);
            resources.image = unsafe { self.runtime.device.create_image(&image_info, None) }
                .context("create Pause Blur scene image")?;
            let requirements = unsafe {
                self.runtime
                    .device
                    .get_image_memory_requirements(resources.image)
            };
            allocation_size = requirements.size;
            let memory_type = self
                .find_memory_type(
                    requirements.memory_type_bits,
                    vk::MemoryPropertyFlags::DEVICE_LOCAL,
                )
                .ok_or_else(|| anyhow!("no device-local memory for Pause Blur scene image"))?;
            let allocate = vk::MemoryAllocateInfo::default()
                .allocation_size(requirements.size)
                .memory_type_index(memory_type);
            resources.memory = unsafe { self.runtime.device.allocate_memory(&allocate, None) }
                .context("allocate Pause Blur scene image")?;
            unsafe {
                self.runtime
                    .device
                    .bind_image_memory(resources.image, resources.memory, 0)
            }
            .context("bind Pause Blur scene image")?;
            let all_levels_range = vk::ImageSubresourceRange::default()
                .aspect_mask(vk::ImageAspectFlags::COLOR)
                .base_mip_level(0)
                .level_count(mip_levels)
                .base_array_layer(0)
                .layer_count(1);
            let view_info = vk::ImageViewCreateInfo::default()
                .image(resources.image)
                .view_type(vk::ImageViewType::TYPE_2D)
                .format(format)
                .subresource_range(all_levels_range);
            resources.all_levels_view =
                unsafe { self.runtime.device.create_image_view(&view_info, None) }
                    .context("create Pause Blur all-levels view")?;
            for level in 0..mip_levels {
                let range = vk::ImageSubresourceRange::default()
                    .aspect_mask(vk::ImageAspectFlags::COLOR)
                    .base_mip_level(level)
                    .level_count(1)
                    .base_array_layer(0)
                    .layer_count(1);
                let view_info = vk::ImageViewCreateInfo::default()
                    .image(resources.image)
                    .view_type(vk::ImageViewType::TYPE_2D)
                    .format(format)
                    .subresource_range(range);
                resources.mip_views.push(
                    unsafe { self.runtime.device.create_image_view(&view_info, None) }
                        .with_context(|| format!("create Pause Blur mip {level} view"))?,
                );
            }
            resources.render_pass = self.create_scene_render_pass(format)?;
            resources.composition_pipeline =
                self.create_pipeline(resources.render_pass, VERTEX_SHADER, FRAGMENT_SHADER)?;
            if mip_levels > 1 {
                resources.downsample_pipeline = self.create_pipeline(
                    resources.render_pass,
                    FULLSCREEN_VERTEX_SHADER,
                    DOWNSAMPLE_FRAGMENT_SHADER,
                )?;
            }
            for (level, view) in resources.mip_views.iter().copied().enumerate() {
                let level_extent = blur_mip_extent(extent, level as u32);
                let attachments = [view];
                let framebuffer_info = vk::FramebufferCreateInfo::default()
                    .render_pass(resources.render_pass)
                    .attachments(&attachments)
                    .width(level_extent.width)
                    .height(level_extent.height)
                    .layers(1);
                resources.framebuffers.push(
                    unsafe {
                        self.runtime
                            .device
                            .create_framebuffer(&framebuffer_info, None)
                    }
                    .with_context(|| format!("create Pause Blur mip {level} framebuffer"))?,
                );
            }
            resources.blur_pipeline = self.create_pipeline(
                swapchain_render_pass,
                FULLSCREEN_VERTEX_SHADER,
                BLUR_FRAGMENT_SHADER,
            )?;

            let set_layouts = vec![self.descriptor_set_layout; mip_levels as usize];
            let allocate_info = vk::DescriptorSetAllocateInfo::default()
                .descriptor_pool(self.descriptor_pool)
                .set_layouts(&set_layouts);
            let mut descriptor_sets =
                unsafe { self.runtime.device.allocate_descriptor_sets(&allocate_info) }
                    .context("allocate Pause Blur descriptor sets")?;
            resources.mix_descriptor_set = descriptor_sets
                .pop()
                .expect("Pause Blur always allocates a mix descriptor set");
            resources.downsample_descriptor_sets = descriptor_sets;
            for (descriptor_set, view) in resources
                .downsample_descriptor_sets
                .iter()
                .zip(resources.mip_views.iter())
            {
                let image_info = [vk::DescriptorImageInfo::default()
                    .sampler(self.sampler)
                    .image_view(*view)
                    .image_layout(vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL)];
                let writes = [vk::WriteDescriptorSet::default()
                    .dst_set(*descriptor_set)
                    .dst_binding(1)
                    .descriptor_type(vk::DescriptorType::COMBINED_IMAGE_SAMPLER)
                    .image_info(&image_info)];
                unsafe { self.runtime.device.update_descriptor_sets(&writes, &[]) };
            }
            let image_info = [vk::DescriptorImageInfo::default()
                .sampler(self.sampler)
                .image_view(resources.all_levels_view)
                .image_layout(vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL)];
            let writes = [vk::WriteDescriptorSet::default()
                .dst_set(resources.mix_descriptor_set)
                .dst_binding(1)
                .descriptor_type(vk::DescriptorType::COMBINED_IMAGE_SAMPLER)
                .image_info(&image_info)];
            unsafe { self.runtime.device.update_descriptor_sets(&writes, &[]) };
            Ok(())
        })();
        if let Err(error) = create {
            self.destroy_blur_resources(resources);
            return Err(error);
        }
        log::debug!(
            "Pause Blur mip scene ready: {}x{} format={:?} levels={} allocation_size={}",
            extent.width,
            extent.height,
            format,
            mip_levels,
            allocation_size
        );
        Ok(resources)
    }

    fn create_scene_render_pass(&self, format: vk::Format) -> Result<vk::RenderPass> {
        let attachment = [vk::AttachmentDescription::default()
            .format(format)
            .samples(vk::SampleCountFlags::TYPE_1)
            .load_op(vk::AttachmentLoadOp::CLEAR)
            .store_op(vk::AttachmentStoreOp::STORE)
            .stencil_load_op(vk::AttachmentLoadOp::DONT_CARE)
            .stencil_store_op(vk::AttachmentStoreOp::DONT_CARE)
            .initial_layout(vk::ImageLayout::UNDEFINED)
            .final_layout(vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL)];
        let color_ref = [vk::AttachmentReference {
            attachment: 0,
            layout: vk::ImageLayout::COLOR_ATTACHMENT_OPTIMAL,
        }];
        let subpass = [vk::SubpassDescription::default()
            .pipeline_bind_point(vk::PipelineBindPoint::GRAPHICS)
            .color_attachments(&color_ref)];
        let dependencies = [
            vk::SubpassDependency::default()
                .src_subpass(vk::SUBPASS_EXTERNAL)
                .dst_subpass(0)
                .src_stage_mask(vk::PipelineStageFlags::FRAGMENT_SHADER)
                .dst_stage_mask(vk::PipelineStageFlags::COLOR_ATTACHMENT_OUTPUT)
                .src_access_mask(vk::AccessFlags::SHADER_READ)
                .dst_access_mask(vk::AccessFlags::COLOR_ATTACHMENT_WRITE),
            vk::SubpassDependency::default()
                .src_subpass(0)
                .dst_subpass(vk::SUBPASS_EXTERNAL)
                .src_stage_mask(vk::PipelineStageFlags::COLOR_ATTACHMENT_OUTPUT)
                .dst_stage_mask(vk::PipelineStageFlags::FRAGMENT_SHADER)
                .src_access_mask(vk::AccessFlags::COLOR_ATTACHMENT_WRITE)
                .dst_access_mask(vk::AccessFlags::SHADER_READ),
        ];
        let info = vk::RenderPassCreateInfo::default()
            .attachments(&attachment)
            .subpasses(&subpass)
            .dependencies(&dependencies);
        unsafe { self.runtime.device.create_render_pass(&info, None) }
            .context("create Pause Blur scene render pass")
    }

    fn destroy_blur_resources(&self, resources: BlurResources) {
        unsafe {
            let mut descriptor_sets = resources.downsample_descriptor_sets;
            if resources.mix_descriptor_set != vk::DescriptorSet::null() {
                descriptor_sets.push(resources.mix_descriptor_set);
            }
            if !descriptor_sets.is_empty() {
                if let Err(error) = self
                    .runtime
                    .device
                    .free_descriptor_sets(self.descriptor_pool, &descriptor_sets)
                {
                    log::warn!("free Pause Blur descriptor sets failed: {error:?}");
                }
            }
            if resources.blur_pipeline != vk::Pipeline::null() {
                self.runtime
                    .device
                    .destroy_pipeline(resources.blur_pipeline, None);
            }
            for framebuffer in resources.framebuffers {
                self.runtime.device.destroy_framebuffer(framebuffer, None);
            }
            if resources.downsample_pipeline != vk::Pipeline::null() {
                self.runtime
                    .device
                    .destroy_pipeline(resources.downsample_pipeline, None);
            }
            if resources.composition_pipeline != vk::Pipeline::null() {
                self.runtime
                    .device
                    .destroy_pipeline(resources.composition_pipeline, None);
            }
            if resources.render_pass != vk::RenderPass::null() {
                self.runtime
                    .device
                    .destroy_render_pass(resources.render_pass, None);
            }
            for view in resources.mip_views {
                self.runtime.device.destroy_image_view(view, None);
            }
            if resources.all_levels_view != vk::ImageView::null() {
                self.runtime
                    .device
                    .destroy_image_view(resources.all_levels_view, None);
            }
            if resources.image != vk::Image::null() {
                self.runtime.device.destroy_image(resources.image, None);
            }
            if resources.memory != vk::DeviceMemory::null() {
                self.runtime.device.free_memory(resources.memory, None);
            }
        }
    }

    fn destroy_current_blur_resources(&mut self) {
        if let Some(resources) = self.blur_resources.take() {
            self.destroy_blur_resources(resources);
        }
    }

    fn create_swapchain_rendering(
        &self,
        images: &[vk::Image],
        format: vk::Format,
        extent: vk::Extent2D,
    ) -> Result<(
        Vec<vk::ImageView>,
        vk::RenderPass,
        vk::Pipeline,
        Vec<vk::Framebuffer>,
    )> {
        let range = vk::ImageSubresourceRange::default()
            .aspect_mask(vk::ImageAspectFlags::COLOR)
            .base_mip_level(0)
            .level_count(1)
            .base_array_layer(0)
            .layer_count(1);
        let mut views = Vec::with_capacity(images.len());
        for image in images {
            let info = vk::ImageViewCreateInfo::default()
                .image(*image)
                .view_type(vk::ImageViewType::TYPE_2D)
                .format(format)
                .subresource_range(range);
            match unsafe { self.runtime.device.create_image_view(&info, None) } {
                Ok(view) => views.push(view),
                Err(error) => {
                    for view in views.drain(..) {
                        unsafe { self.runtime.device.destroy_image_view(view, None) };
                    }
                    return Err(anyhow!("create swapchain image view: {error:?}"));
                }
            }
        }
        let attachment = [vk::AttachmentDescription::default()
            .format(format)
            .samples(vk::SampleCountFlags::TYPE_1)
            .load_op(vk::AttachmentLoadOp::CLEAR)
            .store_op(vk::AttachmentStoreOp::STORE)
            .stencil_load_op(vk::AttachmentLoadOp::DONT_CARE)
            .stencil_store_op(vk::AttachmentStoreOp::DONT_CARE)
            .initial_layout(vk::ImageLayout::UNDEFINED)
            .final_layout(vk::ImageLayout::PRESENT_SRC_KHR)];
        let color_ref = [vk::AttachmentReference {
            attachment: 0,
            layout: vk::ImageLayout::COLOR_ATTACHMENT_OPTIMAL,
        }];
        let subpass = [vk::SubpassDescription::default()
            .pipeline_bind_point(vk::PipelineBindPoint::GRAPHICS)
            .color_attachments(&color_ref)];
        let dependency = [vk::SubpassDependency::default()
            .src_subpass(vk::SUBPASS_EXTERNAL)
            .dst_subpass(0)
            .src_stage_mask(vk::PipelineStageFlags::COLOR_ATTACHMENT_OUTPUT)
            .dst_stage_mask(vk::PipelineStageFlags::COLOR_ATTACHMENT_OUTPUT)
            .dst_access_mask(vk::AccessFlags::COLOR_ATTACHMENT_WRITE)];
        let render_pass_info = vk::RenderPassCreateInfo::default()
            .attachments(&attachment)
            .subpasses(&subpass)
            .dependencies(&dependency);
        let render_pass = unsafe {
            self.runtime
                .device
                .create_render_pass(&render_pass_info, None)
        }
        .context("vkCreateRenderPass")?;
        let pipeline = match self.create_pipeline(render_pass, VERTEX_SHADER, FRAGMENT_SHADER) {
            Ok(pipeline) => pipeline,
            Err(error) => {
                unsafe { self.runtime.device.destroy_render_pass(render_pass, None) };
                for view in views.drain(..) {
                    unsafe { self.runtime.device.destroy_image_view(view, None) };
                }
                return Err(error);
            }
        };
        let mut framebuffers = Vec::with_capacity(views.len());
        for view in &views {
            let attachments = [*view];
            let info = vk::FramebufferCreateInfo::default()
                .render_pass(render_pass)
                .attachments(&attachments)
                .width(extent.width)
                .height(extent.height)
                .layers(1);
            match unsafe { self.runtime.device.create_framebuffer(&info, None) } {
                Ok(framebuffer) => framebuffers.push(framebuffer),
                Err(error) => {
                    for framebuffer in framebuffers.drain(..) {
                        unsafe { self.runtime.device.destroy_framebuffer(framebuffer, None) };
                    }
                    unsafe {
                        self.runtime.device.destroy_pipeline(pipeline, None);
                        self.runtime.device.destroy_render_pass(render_pass, None);
                    }
                    for view in views.drain(..) {
                        unsafe { self.runtime.device.destroy_image_view(view, None) };
                    }
                    return Err(anyhow!("vkCreateFramebuffer: {error:?}"));
                }
            }
        }
        Ok((views, render_pass, pipeline, framebuffers))
    }

    fn cmd_set_render_extent(&self, command_buffer: vk::CommandBuffer, extent: vk::Extent2D) {
        let viewports = [vk::Viewport {
            x: 0.0,
            y: 0.0,
            width: extent.width as f32,
            height: extent.height as f32,
            min_depth: 0.0,
            max_depth: 1.0,
        }];
        let scissors = [vk::Rect2D {
            offset: vk::Offset2D { x: 0, y: 0 },
            extent,
        }];
        unsafe {
            self.runtime
                .device
                .cmd_set_viewport(command_buffer, 0, &viewports);
            self.runtime
                .device
                .cmd_set_scissor(command_buffer, 0, &scissors);
        }
    }

    fn create_pipeline(
        &self,
        render_pass: vk::RenderPass,
        vertex_shader: &[u32],
        fragment_shader: &[u32],
    ) -> Result<vk::Pipeline> {
        let vertex_info = vk::ShaderModuleCreateInfo::default().code(vertex_shader);
        let fragment_info = vk::ShaderModuleCreateInfo::default().code(fragment_shader);
        let vertex = unsafe { self.runtime.device.create_shader_module(&vertex_info, None) }
            .context("create vertex shader")?;
        let fragment = match unsafe {
            self.runtime
                .device
                .create_shader_module(&fragment_info, None)
        } {
            Ok(module) => module,
            Err(error) => {
                unsafe { self.runtime.device.destroy_shader_module(vertex, None) };
                return Err(anyhow!("create fragment shader: {error:?}"));
            }
        };
        let entry = CString::new("main").unwrap();
        let stages = [
            vk::PipelineShaderStageCreateInfo::default()
                .stage(vk::ShaderStageFlags::VERTEX)
                .module(vertex)
                .name(&entry),
            vk::PipelineShaderStageCreateInfo::default()
                .stage(vk::ShaderStageFlags::FRAGMENT)
                .module(fragment)
                .name(&entry),
        ];
        let vertex_input = vk::PipelineVertexInputStateCreateInfo::default();
        let input_assembly = vk::PipelineInputAssemblyStateCreateInfo::default()
            .topology(vk::PrimitiveTopology::TRIANGLE_LIST);
        let viewport = vk::PipelineViewportStateCreateInfo::default()
            .viewport_count(1)
            .scissor_count(1);
        let dynamic_states = [vk::DynamicState::VIEWPORT, vk::DynamicState::SCISSOR];
        let dynamic = vk::PipelineDynamicStateCreateInfo::default().dynamic_states(&dynamic_states);
        let raster = vk::PipelineRasterizationStateCreateInfo::default()
            .polygon_mode(vk::PolygonMode::FILL)
            .cull_mode(vk::CullModeFlags::NONE)
            .front_face(vk::FrontFace::COUNTER_CLOCKWISE)
            .line_width(1.0);
        let multisample = vk::PipelineMultisampleStateCreateInfo::default()
            .rasterization_samples(vk::SampleCountFlags::TYPE_1);
        let color_attachment = [vk::PipelineColorBlendAttachmentState::default()
            .blend_enable(false)
            .color_write_mask(vk::ColorComponentFlags::RGBA)];
        let color_blend =
            vk::PipelineColorBlendStateCreateInfo::default().attachments(&color_attachment);
        let info = [vk::GraphicsPipelineCreateInfo::default()
            .stages(&stages)
            .vertex_input_state(&vertex_input)
            .input_assembly_state(&input_assembly)
            .viewport_state(&viewport)
            .rasterization_state(&raster)
            .multisample_state(&multisample)
            .color_blend_state(&color_blend)
            .dynamic_state(&dynamic)
            .layout(self.pipeline_layout)
            .render_pass(render_pass)
            .subpass(0)];
        let result = unsafe {
            self.runtime
                .device
                .create_graphics_pipelines(vk::PipelineCache::null(), &info, None)
        };
        unsafe {
            self.runtime.device.destroy_shader_module(vertex, None);
            self.runtime.device.destroy_shader_module(fragment, None);
        }
        result
            .map(|pipelines| pipelines[0])
            .map_err(|(_, error)| anyhow!("vkCreateGraphicsPipelines: {error:?}"))
    }

    fn destroy_swapchain_rendering(&mut self) {
        let views = std::mem::take(&mut self.image_views);
        let framebuffers = std::mem::take(&mut self.framebuffers);
        let render_pass = std::mem::replace(&mut self.render_pass, vk::RenderPass::null());
        let pipeline = std::mem::replace(&mut self.pipeline, vk::Pipeline::null());
        self.destroy_swapchain_rendering_parts(views, render_pass, pipeline, framebuffers);
        self.images.clear();
    }

    fn destroy_swapchain_rendering_parts(
        &self,
        views: Vec<vk::ImageView>,
        render_pass: vk::RenderPass,
        pipeline: vk::Pipeline,
        framebuffers: Vec<vk::Framebuffer>,
    ) {
        unsafe {
            for framebuffer in framebuffers {
                self.runtime.device.destroy_framebuffer(framebuffer, None);
            }
            if pipeline != vk::Pipeline::null() {
                self.runtime.device.destroy_pipeline(pipeline, None);
            }
            if render_pass != vk::RenderPass::null() {
                self.runtime.device.destroy_render_pass(render_pass, None);
            }
            for view in views {
                self.runtime.device.destroy_image_view(view, None);
            }
        }
    }
}

impl Drop for WsiPresenter {
    fn drop(&mut self) {
        unsafe {
            let _ = self.runtime.device.device_wait_idle();
        }
        for direct in self.direct_frames.drain(..) {
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
                if let Some(release) = frame.cpu_release_fallback {
                    libc::close(release.release_syncobj_fd);
                }
                if frame.fence != vk::Fence::null() {
                    self.runtime.device.destroy_fence(frame.fence, None);
                }
                if frame.release_finished != vk::Semaphore::null() {
                    self.runtime
                        .device
                        .destroy_semaphore(frame.release_finished, None);
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

type DirectFrameHandles = (
    vk::Image,
    vk::ImageView,
    vk::Extent2D,
    vk::ImageLayout,
    u32,
    vk::Semaphore,
);

fn direct_frame_handles(frame: &DirectFrame) -> DirectFrameHandles {
    (
        frame.image,
        frame.view,
        frame.extent,
        frame.layout,
        frame.external_queue_family,
        frame.acquire_semaphore,
    )
}

fn resolve_direct_release(
    display: *mut sys::waywallen_display_t,
    release: DirectRelease,
) -> Result<()> {
    let rc = unsafe { sys::waywallen_display_signal_release_syncobj(release.release_syncobj_fd) };
    if rc != sys::WAYWALLEN_OK {
        bail!(
            "signal direct frame release generation={} seq={} failed: {rc}",
            release.buffer_generation,
            release.seq
        );
    }
    let rc = unsafe {
        sys::waywallen_display_frame_release_armed(display, release.buffer_generation, release.seq)
    };
    if rc != sys::WAYWALLEN_OK {
        bail!(
            "acknowledge direct frame release generation={} seq={} failed: {rc}",
            release.buffer_generation,
            release.seq
        );
    }
    Ok(())
}

pub fn discard_direct_frame(
    display: *mut sys::waywallen_display_t,
    frame: &sys::waywallen_frame_t,
) -> Result<()> {
    if frame.release_syncobj_fd < 0 {
        return Ok(());
    }
    resolve_direct_release(
        display,
        DirectRelease {
            release_syncobj_fd: frame.release_syncobj_fd,
            buffer_generation: frame.buffer_generation,
            seq: frame.seq,
        },
    )
}

fn full_color_range() -> vk::ImageSubresourceRange {
    vk::ImageSubresourceRange::default()
        .aspect_mask(vk::ImageAspectFlags::COLOR)
        .base_mip_level(0)
        .level_count(1)
        .base_array_layer(0)
        .layer_count(1)
}

fn blur_mip_level_count(extent: vk::Extent2D) -> u32 {
    let mut width = extent.width.max(1);
    let mut height = extent.height.max(1);
    let mut levels = 1;
    while levels < MAX_BLUR_MIP_LEVELS && (width > 1 || height > 1) {
        width = (width / 2).max(1);
        height = (height / 2).max(1);
        levels += 1;
    }
    levels
}

fn blur_mip_extent(extent: vk::Extent2D, level: u32) -> vk::Extent2D {
    vk::Extent2D {
        width: (extent.width >> level).max(1),
        height: (extent.height >> level).max(1),
    }
}

fn blur_weights(radius: f32, mip_levels: u32) -> [f32; 8] {
    let mut weights = [0.0; 8];
    let radius = if radius.is_finite() {
        radius.clamp(0.0, 64.0)
    } else {
        0.0
    };
    if radius <= f32::EPSILON {
        weights[0] = 1.0;
        return weights;
    }

    let lod = (radius / 64.0).sqrt() * 1.2 - 0.2;
    let centers = [0.1, 0.3, 0.5, 0.7, 0.9, 1.1];
    for (weight, center) in weights[..BLUR_WEIGHT_COUNT].iter_mut().zip(centers) {
        *weight = (1.0_f32 - 2.0 * (lod - center).abs()).clamp(0.0, 1.0);
    }
    let sum = weights[..BLUR_WEIGHT_COUNT].iter().sum::<f32>();
    if !sum.is_finite() || sum <= f32::EPSILON {
        weights[0] = 1.0;
        return weights;
    }
    for weight in &mut weights[..BLUR_WEIGHT_COUNT] {
        *weight /= sum;
    }

    let available = mip_levels.clamp(1, MAX_BLUR_MIP_LEVELS) as usize;
    for level in available..BLUR_WEIGHT_COUNT {
        weights[available - 1] += weights[level];
        weights[level] = 0.0;
    }
    weights
}

fn create_release_semaphore(device: &ash::Device) -> Result<vk::Semaphore> {
    let mut export_info = vk::ExportSemaphoreCreateInfo::default()
        .handle_types(vk::ExternalSemaphoreHandleTypeFlags::SYNC_FD);
    let info = vk::SemaphoreCreateInfo::default().push_next(&mut export_info);
    unsafe { device.create_semaphore(&info, None) }
        .context("create direct release export semaphore")
}

fn choose_surface_format(formats: &[vk::SurfaceFormatKHR]) -> vk::SurfaceFormatKHR {
    if formats.len() == 1 && formats[0].format == vk::Format::UNDEFINED {
        return vk::SurfaceFormatKHR {
            format: vk::Format::B8G8R8A8_UNORM,
            color_space: formats[0].color_space,
        };
    }
    formats
        .iter()
        .copied()
        .find(|format| {
            matches!(
                format.format,
                vk::Format::B8G8R8A8_UNORM | vk::Format::R8G8B8A8_UNORM
            ) && format.color_space == vk::ColorSpaceKHR::SRGB_NONLINEAR
        })
        .unwrap_or(formats[0])
}

fn choose_image_count(capabilities: &vk::SurfaceCapabilitiesKHR) -> u32 {
    let mut count = capabilities.min_image_count.max(FRAMES_IN_FLIGHT as u32);
    if capabilities.max_image_count > 0 {
        count = count.min(capabilities.max_image_count);
    }
    count
}

fn choose_extent(
    capabilities: &vk::SurfaceCapabilitiesKHR,
    requested: vk::Extent2D,
) -> vk::Extent2D {
    if capabilities.current_extent.width != u32::MAX {
        return capabilities.current_extent;
    }
    vk::Extent2D {
        width: requested.width.clamp(
            capabilities.min_image_extent.width,
            capabilities.max_image_extent.width,
        ),
        height: requested.height.clamp(
            capabilities.min_image_extent.height,
            capabilities.max_image_extent.height,
        ),
    }
}

fn choose_composite_alpha(supported: vk::CompositeAlphaFlagsKHR) -> vk::CompositeAlphaFlagsKHR {
    [
        vk::CompositeAlphaFlagsKHR::OPAQUE,
        vk::CompositeAlphaFlagsKHR::PRE_MULTIPLIED,
        vk::CompositeAlphaFlagsKHR::POST_MULTIPLIED,
        vk::CompositeAlphaFlagsKHR::INHERIT,
    ]
    .into_iter()
    .find(|mode| supported.contains(*mode))
    .unwrap_or(vk::CompositeAlphaFlagsKHR::OPAQUE)
}

fn composition_push_constants(
    composition: &Composition,
    output: vk::Extent2D,
    source_image: vk::Extent2D,
) -> CompositionPushConstants {
    let [x, y, width, height] = composition.destination;
    let swaps_dimensions = matches!(composition.transform, 1 | 3 | 5 | 7);
    let (pre_width, pre_height) = if swaps_dimensions {
        (output.height as f32, output.width as f32)
    } else {
        (output.width as f32, output.height as f32)
    };
    let pre_corners = [
        [x / pre_width, y / pre_height],
        [(x + width) / pre_width, y / pre_height],
        [x / pre_width, (y + height) / pre_height],
    ];
    let positions = pre_corners.map(|[u, v]| {
        let [display_u, display_v] = forward_display(composition.transform, u, v);
        [display_u * 2.0 - 1.0, display_v * 2.0 - 1.0]
    });
    let [sx, sy, sw, sh] = composition.source;
    let u0 = sx / source_image.width as f32;
    let v0 = sy / source_image.height as f32;
    CompositionPushConstants {
        position_origin: [positions[0][0], positions[0][1], 0.0, 0.0],
        position_axes: [
            positions[1][0] - positions[0][0],
            positions[1][1] - positions[0][1],
            positions[2][0] - positions[0][0],
            positions[2][1] - positions[0][1],
        ],
        uv_origin_scale: [
            u0,
            v0,
            sw / source_image.width as f32,
            sh / source_image.height as f32,
        ],
    }
}

fn forward_display(transform: u32, pre_u: f32, pre_v: f32) -> [f32; 2] {
    match transform {
        1 => [1.0 - pre_v, pre_u],
        2 => [1.0 - pre_u, 1.0 - pre_v],
        3 => [pre_v, 1.0 - pre_u],
        4 => [1.0 - pre_u, pre_v],
        5 => [pre_v, pre_u],
        6 => [pre_u, 1.0 - pre_v],
        7 => [1.0 - pre_v, 1.0 - pre_u],
        _ => [pre_u, pre_v],
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn assert_near(actual: f32, expected: f32) {
        assert!((actual - expected).abs() < 0.001, "{actual} != {expected}");
    }

    #[test]
    fn initial_blur_target_is_installed_without_animation() {
        let now = Instant::now();
        let mut transition = BlurTransition::default();
        assert!(transition.set_target(30.0, now, false));
        let sample = transition.sample(now);
        assert_near(sample.radius, 30.0);
        assert!(!sample.animating);
        assert!(!sample.finished);
    }

    #[test]
    fn blur_transition_uses_out_cubic_and_finishes_exactly() {
        let now = Instant::now();
        let mut transition = BlurTransition::default();
        assert!(transition.set_target(64.0, now, true));
        let midpoint = transition.sample(now + BLUR_TRANSITION_DURATION / 2);
        assert_near(midpoint.radius, 56.0);
        assert!(midpoint.animating);
        let endpoint = transition.sample(now + BLUR_TRANSITION_DURATION);
        assert_near(endpoint.radius, 64.0);
        assert!(!endpoint.animating);
        assert!(endpoint.finished);
    }

    #[test]
    fn blur_transition_reverses_from_the_interpolated_value() {
        let now = Instant::now();
        let midpoint = now + BLUR_TRANSITION_DURATION / 2;
        let mut transition = BlurTransition::default();
        transition.set_target(64.0, now, true);
        assert!(transition.set_target(0.0, midpoint, true));
        assert_near(transition.sample(midpoint).radius, 56.0);
        assert_near(
            transition
                .sample(midpoint + BLUR_TRANSITION_DURATION / 2)
                .radius,
            7.0,
        );
    }

    #[test]
    fn pause_presentation_only_targets_radius_when_active() {
        assert_eq!(
            PausePresentation {
                configured: true,
                active: false,
                radius: 42,
            }
            .target_radius(),
            0.0
        );
        assert_eq!(
            PausePresentation {
                configured: true,
                active: true,
                radius: 42,
            }
            .target_radius(),
            42.0
        );
    }

    #[test]
    fn configured_blur_keeps_the_persistent_scene_while_inactive() {
        let presentation = PausePresentation {
            configured: true,
            active: false,
            radius: 30,
        };
        let idle = BlurSample {
            radius: 0.0,
            animating: false,
            finished: false,
        };
        assert!(needs_persistent_scene(true, presentation, &idle, false));
        assert!(!needs_persistent_scene(
            true,
            PausePresentation::default(),
            &idle,
            true
        ));
    }

    #[test]
    fn blur_mips_stop_at_six_and_clamp_each_dimension() {
        let extent = vk::Extent2D {
            width: 3436,
            height: 1440,
        };
        assert_eq!(blur_mip_level_count(extent), 6);
        assert_eq!(
            blur_mip_extent(extent, 5),
            vk::Extent2D {
                width: 107,
                height: 45,
            }
        );
        assert_eq!(
            blur_mip_level_count(vk::Extent2D {
                width: 1,
                height: 3,
            }),
            2
        );
        assert_eq!(
            blur_mip_extent(
                vk::Extent2D {
                    width: 1,
                    height: 3,
                },
                1,
            ),
            vk::Extent2D {
                width: 1,
                height: 1,
            }
        );
    }

    #[test]
    fn blur_weights_are_normalized_for_supported_radii() {
        for radius in [0.0, 1.0, 30.0, 64.0] {
            let weights = blur_weights(radius, MAX_BLUR_MIP_LEVELS);
            assert!(weights.iter().all(|weight| weight.is_finite()));
            assert!(weights.iter().all(|weight| *weight >= 0.0));
            assert_near(weights.iter().sum(), 1.0);
        }
        assert_eq!(blur_weights(0.0, MAX_BLUR_MIP_LEVELS)[0], 1.0);
    }

    #[test]
    fn unavailable_blur_levels_merge_into_the_deepest_mip() {
        let full = blur_weights(64.0, MAX_BLUR_MIP_LEVELS);
        let reduced = blur_weights(64.0, 3);
        assert_near(reduced[0], full[0]);
        assert_near(reduced[1], full[1]);
        assert_near(reduced[2], full[2..BLUR_WEIGHT_COUNT].iter().sum());
        assert!(reduced[3..].iter().all(|weight| *weight == 0.0));
    }

    #[test]
    fn completed_blur_exit_keeps_the_scene_for_its_final_draw() {
        let finished = BlurSample {
            radius: 0.0,
            animating: false,
            finished: true,
        };
        assert!(needs_persistent_scene(
            true,
            PausePresentation::default(),
            &finished,
            true
        ));
        assert!(!needs_persistent_scene(
            true,
            PausePresentation::default(),
            &finished,
            false
        ));
    }

    #[test]
    fn composition_uses_destination_and_source_rects() {
        let push = composition_push_constants(
            &Composition {
                source: [10.0, 20.0, 100.0, 50.0],
                destination: [100.0, 50.0, 200.0, 100.0],
                transform: 0,
                clear: [0.0; 4],
            },
            vk::Extent2D {
                width: 400,
                height: 200,
            },
            vk::Extent2D {
                width: 200,
                height: 100,
            },
        );
        assert_eq!(push.position_origin, [-0.5, -0.5, 0.0, 0.0]);
        assert_eq!(push.position_axes, [1.0, 0.0, 0.0, 1.0]);
        assert_eq!(push.uv_origin_scale, [0.05, 0.2, 0.5, 0.5]);
    }

    #[test]
    fn clockwise_rotation_maps_pre_transform_space_to_display() {
        let push = composition_push_constants(
            &Composition {
                source: [0.0, 0.0, 200.0, 100.0],
                destination: [0.0, 0.0, 200.0, 100.0],
                transform: 1,
                clear: [0.0; 4],
            },
            vk::Extent2D {
                width: 100,
                height: 200,
            },
            vk::Extent2D {
                width: 200,
                height: 100,
            },
        );
        assert_eq!(push.position_origin, [1.0, -1.0, 0.0, 0.0]);
        assert_eq!(push.position_axes, [0.0, 2.0, -2.0, 0.0]);
        assert_eq!(push.uv_origin_scale, [0.0, 0.0, 1.0, 1.0]);
    }

    #[test]
    fn swapchain_choices_obey_surface_limits() {
        let capabilities = vk::SurfaceCapabilitiesKHR {
            min_image_count: 1,
            max_image_count: 2,
            current_extent: vk::Extent2D {
                width: u32::MAX,
                height: u32::MAX,
            },
            min_image_extent: vk::Extent2D {
                width: 320,
                height: 200,
            },
            max_image_extent: vk::Extent2D {
                width: 3840,
                height: 2160,
            },
            ..Default::default()
        };
        assert_eq!(choose_image_count(&capabilities), 2);
        assert_eq!(
            choose_extent(
                &capabilities,
                vk::Extent2D {
                    width: 8_000,
                    height: 100,
                }
            ),
            vk::Extent2D {
                width: 3840,
                height: 200,
            }
        );
    }

    #[test]
    fn surface_format_handles_undefined_offer() {
        let selected = choose_surface_format(&[vk::SurfaceFormatKHR {
            format: vk::Format::UNDEFINED,
            color_space: vk::ColorSpaceKHR::SRGB_NONLINEAR,
        }]);
        assert_eq!(selected.format, vk::Format::B8G8R8A8_UNORM);
        assert_eq!(selected.color_space, vk::ColorSpaceKHR::SRGB_NONLINEAR);
    }

    #[test]
    fn drm_match_precedes_queue_shape_for_device_ranking() {
        assert!(device_candidate_sort_key(true, false) < device_candidate_sort_key(false, true));
        assert!(device_candidate_sort_key(true, true) < device_candidate_sort_key(true, false));
    }

    #[test]
    fn unified_graphics_present_queue_is_preferred() {
        assert_eq!(choose_queue_families(&[1, 3], &[2, 3]), Some((3, 3)));
        assert_eq!(choose_queue_families(&[1], &[2]), Some((1, 2)));
        assert_eq!(choose_queue_families(&[], &[2]), None);
    }

    #[test]
    fn every_wire_transform_has_the_expected_forward_mapping() {
        let point = [0.2, 0.7];
        let expected = [
            [0.2, 0.7],
            [0.3, 0.2],
            [0.8, 0.3],
            [0.7, 0.8],
            [0.8, 0.7],
            [0.7, 0.2],
            [0.2, 0.3],
            [0.3, 0.8],
        ];
        for (transform, expected) in expected.into_iter().enumerate() {
            assert_eq!(
                forward_display(transform as u32, point[0], point[1]),
                expected
            );
        }
    }
}
