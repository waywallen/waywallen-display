use std::collections::HashSet;
use std::ffi::{c_void, CStr, CString};
use std::sync::Arc;

use anyhow::{anyhow, bail, Context, Result};
use ash::vk::{self, Handle};
use wayland_client::protocol::wl_surface::WlSurface;
use wayland_client::{Connection, Proxy};
use waywallen_display as sys;

const FRAMES_IN_FLIGHT: usize = 2;
const PUSH_CONSTANT_SIZE: u32 = 6 * 4 * 4;
const RESOURCE_RETIRE_TIMEOUT_NS: u64 = 2_000_000_000;

include!(concat!(env!("OUT_DIR"), "/layer_shell_shaders.rs"));

pub struct VulkanRuntime {
    _entry: ash::Entry,
    instance: ash::Instance,
    surface_loader: ash::khr::surface::Instance,
    wayland_surface_loader: ash::khr::wayland_surface::Instance,
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

struct FrameContext {
    command_pool: vk::CommandPool,
    command_buffer: vk::CommandBuffer,
    image_available: vk::Semaphore,
    render_finished: vk::Semaphore,
    fence: vk::Fence,
    descriptor_set: vk::DescriptorSet,
}

pub struct WsiPresenter {
    runtime: Arc<VulkanRuntime>,
    surface: vk::SurfaceKHR,
    swapchain: vk::SwapchainKHR,
    format: vk::Format,
    extent: vk::Extent2D,
    images: Vec<vk::Image>,
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
    sampled_image: vk::Image,
    sampled_view: vk::ImageView,
    sampled_format: vk::Format,
    sampled_layout: vk::ImageLayout,
    sampled_extent: vk::Extent2D,
    recreate_extent: Option<vk::Extent2D>,
    retired_swapchains: Vec<vk::SwapchainKHR>,
}

pub enum PresentResult {
    Presented,
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
            sampled_image: vk::Image::null(),
            sampled_view: vk::ImageView::null(),
            sampled_format: vk::Format::UNDEFINED,
            sampled_layout: vk::ImageLayout::UNDEFINED,
            sampled_extent: vk::Extent2D::default(),
            recreate_extent: None,
            retired_swapchains: Vec::new(),
        };
        presenter.initialize(extent)?;
        Ok(presenter)
    }

    fn initialize(&mut self, extent: (u32, u32)) -> Result<()> {
        let descriptor_binding = [vk::DescriptorSetLayoutBinding::default()
            .binding(0)
            .descriptor_type(vk::DescriptorType::COMBINED_IMAGE_SAMPLER)
            .descriptor_count(1)
            .stage_flags(vk::ShaderStageFlags::FRAGMENT)];
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
            descriptor_count: FRAMES_IN_FLIGHT as u32,
        }];
        let pool_info = vk::DescriptorPoolCreateInfo::default()
            .max_sets(FRAMES_IN_FLIGHT as u32)
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
        let push_range = [vk::PushConstantRange::default()
            .stage_flags(vk::ShaderStageFlags::VERTEX)
            .offset(0)
            .size(PUSH_CONSTANT_SIZE)];
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
            .max_lod(0.0);
        self.sampler = unsafe { self.runtime.device.create_sampler(&sampler_info, None) }
            .context("vkCreateSampler")?;

        for descriptor_set in descriptor_sets {
            self.frames.push(FrameContext {
                command_pool: vk::CommandPool::null(),
                command_buffer: vk::CommandBuffer::null(),
                image_available: vk::Semaphore::null(),
                render_finished: vk::Semaphore::null(),
                fence: vk::Fence::null(),
                descriptor_set,
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
            frame.render_finished =
                unsafe { self.runtime.device.create_semaphore(&semaphore_info, None) }
                    .context("create render-finished semaphore")?;
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

    pub fn install_sampled_frame(
        &mut self,
        display: *mut sys::waywallen_display_t,
        frame: &sys::waywallen_vk_sampled_frame_t,
    ) -> Result<()> {
        let image = vk::Image::from_raw(frame.image as usize as u64);
        let format = vk::Format::from_raw(frame.format as i32);
        let extent = vk::Extent2D {
            width: frame.width,
            height: frame.height,
        };
        if image == self.sampled_image {
            return Ok(());
        }
        let range = vk::ImageSubresourceRange::default()
            .aspect_mask(vk::ImageAspectFlags::COLOR)
            .base_mip_level(0)
            .level_count(1)
            .base_array_layer(0)
            .layer_count(1);
        let view_info = vk::ImageViewCreateInfo::default()
            .image(image)
            .view_type(vk::ImageViewType::TYPE_2D)
            .format(format)
            .subresource_range(range);
        let candidate_view = match unsafe {
            self.runtime.device.create_image_view(&view_info, None)
        } {
            Ok(view) => view,
            Err(error) => {
                if frame.candidate {
                    let _ = unsafe { sys::waywallen_display_vulkan_discard_sampled_frame(display) };
                }
                return Err(anyhow!("create sampled image view: {error:?}"));
            }
        };
        if frame.candidate {
            if self.sampled_view != vk::ImageView::null() {
                if let Err(error) = self.wait_frames_idle() {
                    unsafe { self.runtime.device.destroy_image_view(candidate_view, None) };
                    let _ = unsafe { sys::waywallen_display_vulkan_discard_sampled_frame(display) };
                    return Err(error.context("retire previous sampled Vulkan frame"));
                }
                unsafe {
                    self.runtime
                        .device
                        .destroy_image_view(self.sampled_view, None)
                };
            }
            let rc = unsafe { sys::waywallen_display_vulkan_commit_sampled_frame(display) };
            if rc != sys::WAYWALLEN_OK {
                unsafe { self.runtime.device.destroy_image_view(candidate_view, None) };
                let _ = unsafe { sys::waywallen_display_vulkan_discard_sampled_frame(display) };
                self.sampled_view = vk::ImageView::null();
                self.sampled_image = vk::Image::null();
                bail!("commit sampled Vulkan frame failed: {rc}");
            }
        } else if self.sampled_view != vk::ImageView::null() {
            unsafe {
                self.runtime
                    .device
                    .destroy_image_view(self.sampled_view, None)
            };
        }
        self.sampled_image = image;
        self.sampled_view = candidate_view;
        self.sampled_format = format;
        self.sampled_layout = vk::ImageLayout::from_raw(frame.layout as i32);
        self.sampled_extent = extent;
        Ok(())
    }

    pub fn clear_sampled_frame(&mut self) -> Result<bool> {
        if !self.frames_idle()? {
            return Ok(false);
        }
        self.destroy_sampled_view();
        Ok(true)
    }

    pub fn force_clear_sampled_frame(&mut self) {
        if self.sampled_view != vk::ImageView::null() {
            unsafe {
                let _ = self.runtime.device.device_wait_idle();
            }
            self.destroy_sampled_view();
        }
    }

    fn destroy_sampled_view(&mut self) {
        if self.sampled_view == vk::ImageView::null() {
            return;
        }
        unsafe {
            self.runtime
                .device
                .destroy_image_view(self.sampled_view, None);
        }
        self.sampled_view = vk::ImageView::null();
        self.sampled_image = vk::Image::null();
        self.sampled_format = vk::Format::UNDEFINED;
        self.sampled_layout = vk::ImageLayout::UNDEFINED;
        self.sampled_extent = vk::Extent2D::default();
    }

    pub fn present(&mut self, composition: &Composition) -> Result<PresentResult> {
        if self.sampled_view == vk::ImageView::null() {
            return Ok(PresentResult::Presented);
        }
        if let Some(extent) = self.recreate_extent {
            if !self.frames_idle()? {
                return Ok(PresentResult::Pending);
            }
            self.recreate_extent = None;
            self.recreate_swapchain(extent)?;
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

        let image_info = [vk::DescriptorImageInfo::default()
            .sampler(self.sampler)
            .image_view(self.sampled_view)
            .image_layout(self.sampled_layout)];
        let writes = [vk::WriteDescriptorSet::default()
            .dst_set(frame.descriptor_set)
            .dst_binding(0)
            .descriptor_type(vk::DescriptorType::COMBINED_IMAGE_SAMPLER)
            .image_info(&image_info)];
        unsafe { self.runtime.device.update_descriptor_sets(&writes, &[]) };

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
        let render_area = vk::Rect2D {
            offset: vk::Offset2D { x: 0, y: 0 },
            extent: self.extent,
        };
        let render_begin = vk::RenderPassBeginInfo::default()
            .render_pass(self.render_pass)
            .framebuffer(self.framebuffers[image_index as usize])
            .render_area(render_area)
            .clear_values(&clear);
        let vertices = composition_vertices(composition, self.extent, self.sampled_extent);
        let vertex_bytes = unsafe {
            std::slice::from_raw_parts(
                vertices.as_ptr().cast::<u8>(),
                std::mem::size_of_val(&vertices),
            )
        };
        unsafe {
            self.runtime.device.cmd_begin_render_pass(
                frame.command_buffer,
                &render_begin,
                vk::SubpassContents::INLINE,
            );
            self.runtime.device.cmd_bind_pipeline(
                frame.command_buffer,
                vk::PipelineBindPoint::GRAPHICS,
                self.pipeline,
            );
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
                vertex_bytes,
            );
            self.runtime
                .device
                .cmd_draw(frame.command_buffer, 6, 1, 0, 0);
            self.runtime
                .device
                .cmd_end_render_pass(frame.command_buffer);
            self.runtime.device.end_command_buffer(frame.command_buffer)
        }
        .context("record WSI command buffer")?;

        let wait_semaphores = [frame.image_available];
        let wait_stages = [vk::PipelineStageFlags::COLOR_ATTACHMENT_OUTPUT];
        let command_buffers = [frame.command_buffer];
        let signal_semaphores = [frame.render_finished];
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
        let swapchains = [self.swapchain];
        let image_indices = [image_index];
        let present_info = vk::PresentInfoKHR::default()
            .wait_semaphores(&signal_semaphores)
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
        self.frame_cursor = (self.frame_cursor + 1) % self.frames.len();
        Ok(PresentResult::Presented)
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
        let (new_views, new_render_pass, new_pipeline, new_framebuffers) =
            match self.create_swapchain_rendering(&new_images, surface_format.format, extent) {
                Ok(resources) => resources,
                Err(error) => {
                    unsafe {
                        self.runtime
                            .swapchain_loader
                            .destroy_swapchain(new_swapchain, None)
                    };
                    return Err(error);
                }
            };

        self.destroy_swapchain_rendering();
        if self.swapchain != vk::SwapchainKHR::null() {
            self.retired_swapchains.push(self.swapchain);
        }
        self.swapchain = new_swapchain;
        self.images = new_images;
        self.image_views = new_views;
        self.render_pass = new_render_pass;
        self.pipeline = new_pipeline;
        self.framebuffers = new_framebuffers;
        self.format = surface_format.format;
        self.extent = extent;
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
        let pipeline = match self.create_pipeline(render_pass, extent) {
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

    fn create_pipeline(
        &self,
        render_pass: vk::RenderPass,
        extent: vk::Extent2D,
    ) -> Result<vk::Pipeline> {
        let vertex_info = vk::ShaderModuleCreateInfo::default().code(VERTEX_SHADER);
        let fragment_info = vk::ShaderModuleCreateInfo::default().code(FRAGMENT_SHADER);
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
        let viewport = vk::PipelineViewportStateCreateInfo::default()
            .viewports(&viewports)
            .scissors(&scissors);
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
        unsafe {
            for framebuffer in self.framebuffers.drain(..) {
                self.runtime.device.destroy_framebuffer(framebuffer, None);
            }
            if self.pipeline != vk::Pipeline::null() {
                self.runtime.device.destroy_pipeline(self.pipeline, None);
                self.pipeline = vk::Pipeline::null();
            }
            if self.render_pass != vk::RenderPass::null() {
                self.runtime
                    .device
                    .destroy_render_pass(self.render_pass, None);
                self.render_pass = vk::RenderPass::null();
            }
            for view in self.image_views.drain(..) {
                self.runtime.device.destroy_image_view(view, None);
            }
        }
        self.images.clear();
    }
}

impl Drop for WsiPresenter {
    fn drop(&mut self) {
        unsafe {
            let _ = self.runtime.device.device_wait_idle();
        }
        self.destroy_sampled_view();
        self.destroy_swapchain_rendering();
        unsafe {
            if self.swapchain != vk::SwapchainKHR::null() {
                self.runtime
                    .swapchain_loader
                    .destroy_swapchain(self.swapchain, None);
            }
            for swapchain in self.retired_swapchains.drain(..) {
                self.runtime
                    .swapchain_loader
                    .destroy_swapchain(swapchain, None);
            }
            for frame in self.frames.drain(..) {
                if frame.fence != vk::Fence::null() {
                    self.runtime.device.destroy_fence(frame.fence, None);
                }
                if frame.render_finished != vk::Semaphore::null() {
                    self.runtime
                        .device
                        .destroy_semaphore(frame.render_finished, None);
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

fn composition_vertices(
    composition: &Composition,
    output: vk::Extent2D,
    source_image: vk::Extent2D,
) -> [[f32; 4]; 6] {
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
        [(x + width) / pre_width, (y + height) / pre_height],
    ];
    let positions = pre_corners.map(|[u, v]| {
        let [display_u, display_v] = forward_display(composition.transform, u, v);
        [display_u * 2.0 - 1.0, display_v * 2.0 - 1.0]
    });
    let [sx, sy, sw, sh] = composition.source;
    let u0 = sx / source_image.width as f32;
    let v0 = sy / source_image.height as f32;
    let u1 = (sx + sw) / source_image.width as f32;
    let v1 = (sy + sh) / source_image.height as f32;
    let uv = [[u0, v0], [u1, v0], [u0, v1], [u1, v1]];
    [
        [positions[0][0], positions[0][1], uv[0][0], uv[0][1]],
        [positions[1][0], positions[1][1], uv[1][0], uv[1][1]],
        [positions[2][0], positions[2][1], uv[2][0], uv[2][1]],
        [positions[2][0], positions[2][1], uv[2][0], uv[2][1]],
        [positions[1][0], positions[1][1], uv[1][0], uv[1][1]],
        [positions[3][0], positions[3][1], uv[3][0], uv[3][1]],
    ]
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

    #[test]
    fn composition_uses_destination_and_source_rects() {
        let vertices = composition_vertices(
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
        assert_eq!(vertices[0], [-0.5, -0.5, 0.05, 0.2]);
        assert_eq!(vertices[5], [0.5, 0.5, 0.55, 0.7]);
    }

    #[test]
    fn clockwise_rotation_maps_pre_transform_space_to_display() {
        let vertices = composition_vertices(
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
        assert_eq!(vertices[0], [1.0, -1.0, 0.0, 0.0]);
        assert_eq!(vertices[5], [-1.0, 1.0, 1.0, 1.0]);
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
