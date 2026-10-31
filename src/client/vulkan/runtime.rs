use super::VulkanRuntime;
use crate as sys;
use anyhow::{anyhow, bail, Context, Result};
use ash::vk;
use ash::vk::Handle;
use std::collections::HashSet;
use std::ffi::{c_char, c_void, CStr, CString};
use std::sync::Arc;

/// The window-system half of presentation. Each client implements it for its
/// own connection (Wayland, XCB); everything else in this module is shared.
pub trait WindowSystem {
    /// What a single Vulkan surface is created from.
    type Target;
    /// Shown in log and error messages, e.g. "Wayland".
    const NAME: &'static str;
    const APPLICATION: &'static CStr;
    /// Instance extension providing the surface entry points.
    const SURFACE_EXTENSION: &'static CStr;

    fn create_surface(
        &self,
        entry: &ash::Entry,
        instance: &ash::Instance,
        target: &Self::Target,
    ) -> Result<vk::SurfaceKHR>;

    fn presentation_support(
        &self,
        entry: &ash::Entry,
        instance: &ash::Instance,
        physical_device: vk::PhysicalDevice,
        queue_family: u32,
    ) -> bool;
}

impl VulkanRuntime {
    pub fn new<W: WindowSystem>(
        system: &W,
        surfaces: &[(u32, W::Target)],
        compositor_drm: (u32, u32),
    ) -> Result<(Arc<Self>, Vec<(u32, vk::SurfaceKHR)>)> {
        if surfaces.is_empty() {
            bail!("cannot initialize Vulkan without a {} surface", W::NAME);
        }
        let requirements = importer_requirements()?;
        let entry = unsafe { ash::Entry::load() }.context("load Vulkan loader")?;
        let app_name = W::APPLICATION;
        let app_info = vk::ApplicationInfo::default()
            .application_name(app_name)
            .application_version(1)
            .engine_name(app_name)
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
            W::SURFACE_EXTENSION.as_ptr(),
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

        let mut vk_surfaces = Vec::with_capacity(surfaces.len());
        for (output, surface) in surfaces {
            match system.create_surface(&entry, &instance, surface) {
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
            &entry,
            &instance,
            &surface_loader,
            system,
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
        let mut device_extensions: Vec<*const c_char> = requirements
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
                entry,
                instance,
                surface_loader,
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

    pub fn create_surface<W: WindowSystem>(
        &self,
        system: &W,
        surface: &W::Target,
    ) -> Result<vk::SurfaceKHR> {
        let vk_surface = system.create_surface(&self.entry, &self.instance, surface)?;
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
            bail!(
                "selected Vulkan device cannot present to the hot-plugged {} surface",
                W::NAME
            );
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
                std::mem::transmute(self.entry.static_fn().get_instance_proc_addr)
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

fn select_physical_device<W: WindowSystem>(
    entry: &ash::Entry,
    instance: &ash::Instance,
    surface_loader: &ash::khr::surface::Instance,
    system: &W,
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
            if !system.presentation_support(entry, instance, physical_device, family) {
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
                "no Vulkan device satisfies display DMA-BUF import requirements and all {} targets",
                W::NAME
            )
        })
}

pub(super) fn device_candidate_sort_key(drm_match: bool, unified_queue: bool) -> (bool, bool) {
    (!drm_match, !unified_queue)
}

pub(super) fn choose_queue_families(graphics: &[u32], present: &[u32]) -> Option<(u32, u32)> {
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
