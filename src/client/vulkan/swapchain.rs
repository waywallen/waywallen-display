use super::{
    RetiredSwapchain, WsiPresenter, BLUR_FRAGMENT_SHADER, FRAMES_IN_FLIGHT,
    FULLSCREEN_VERTEX_SHADER, TRANSITION_FRAGMENT_SHADER,
};
use anyhow::{anyhow, bail, Context, Result};
use ash::vk;

impl WsiPresenter {
    pub(super) fn recreate_swapchain(&mut self, requested: vk::Extent2D) -> Result<()> {
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
            bail!("surface has no Vulkan formats");
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

        let new_transition_pipeline = if self.transition_resources.is_some() {
            match self.create_pipeline(
                new_render_pass,
                FULLSCREEN_VERTEX_SHADER,
                TRANSITION_FRAGMENT_SHADER,
            ) {
                Ok(pipeline) => Some(pipeline),
                Err(error) => {
                    if let Some(pipeline) = new_blur_pipeline {
                        unsafe { self.runtime.device.destroy_pipeline(pipeline, None) };
                    }
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
                    return Err(error.context("recreate transition output pipeline"));
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
        if let (Some(resources), Some(new_pipeline)) =
            (self.transition_resources.as_mut(), new_transition_pipeline)
        {
            let old_pipeline = std::mem::replace(&mut resources.output_pipeline, new_pipeline);
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
        self.blank_state.invalidate_swapchain();
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
}

pub(super) fn choose_surface_format(formats: &[vk::SurfaceFormatKHR]) -> vk::SurfaceFormatKHR {
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

pub(super) fn choose_image_count(capabilities: &vk::SurfaceCapabilitiesKHR) -> u32 {
    let mut count = capabilities.min_image_count.max(FRAMES_IN_FLIGHT as u32);
    if capabilities.max_image_count > 0 {
        count = count.min(capabilities.max_image_count);
    }
    count
}

pub(super) fn choose_extent(
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
