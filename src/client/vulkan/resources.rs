use super::effects::{blur_mip_extent, blur_mip_level_count};
use super::{
    full_color_range, BlurResources, TransitionResources, TransitionTarget, WsiPresenter,
    BLUR_FRAGMENT_SHADER, DOWNSAMPLE_FRAGMENT_SHADER, FRAGMENT_SHADER, FULLSCREEN_VERTEX_SHADER,
    TRANSITION_FRAGMENT_SHADER, TRANSITION_TARGET_COUNT, VERTEX_SHADER,
};
use anyhow::{anyhow, Context, Result};
use ash::vk;

impl WsiPresenter {
    pub(super) fn choose_scene_format(&self, swapchain_format: vk::Format) -> Option<vk::Format> {
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

    pub(super) fn create_blur_resources(
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
            format,
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

    pub(super) fn destroy_current_blur_resources(&mut self) {
        // Transition targets sample the scene, so they never outlive it.
        self.active_transition = None;
        if let Some(resources) = self.transition_resources.take() {
            self.destroy_transition_resources(resources);
        }
        if let Some(resources) = self.blur_resources.take() {
            self.destroy_blur_resources(resources);
        }
    }

    pub(super) fn create_transition_resources(&self) -> Result<TransitionResources> {
        let scene = self
            .blur_resources
            .as_ref()
            .ok_or_else(|| anyhow!("transition targets require a persistent scene"))?;
        let mut resources = TransitionResources {
            targets: Vec::with_capacity(TRANSITION_TARGET_COUNT),
            front: 0,
            capture_pipeline: vk::Pipeline::null(),
            capture_scene_pipeline: vk::Pipeline::null(),
            output_pipeline: vk::Pipeline::null(),
        };
        let create = (|| -> Result<()> {
            for index in 0..TRANSITION_TARGET_COUNT {
                let mut target = TransitionTarget {
                    image: vk::Image::null(),
                    memory: vk::DeviceMemory::null(),
                    view: vk::ImageView::null(),
                    framebuffer: vk::Framebuffer::null(),
                    descriptor_set: vk::DescriptorSet::null(),
                };
                let result = self.create_transition_target(scene, &mut target);
                resources.targets.push(target);
                result.with_context(|| format!("create transition target {index}"))?;
            }
            resources.capture_pipeline = self.create_pipeline(
                scene.render_pass,
                FULLSCREEN_VERTEX_SHADER,
                TRANSITION_FRAGMENT_SHADER,
            )?;
            resources.capture_scene_pipeline = self.create_pipeline(
                scene.render_pass,
                FULLSCREEN_VERTEX_SHADER,
                BLUR_FRAGMENT_SHADER,
            )?;
            resources.output_pipeline = self.create_pipeline(
                self.render_pass,
                FULLSCREEN_VERTEX_SHADER,
                TRANSITION_FRAGMENT_SHADER,
            )?;
            Ok(())
        })();
        if let Err(error) = create {
            self.destroy_transition_resources(resources);
            return Err(error);
        }
        log::debug!(
            "transition targets ready: {}x{} format={:?}",
            scene.extent.width,
            scene.extent.height,
            scene.format
        );
        Ok(resources)
    }

    fn create_transition_target(
        &self,
        scene: &BlurResources,
        target: &mut TransitionTarget,
    ) -> Result<()> {
        let image_info = vk::ImageCreateInfo::default()
            .image_type(vk::ImageType::TYPE_2D)
            .format(scene.format)
            .extent(vk::Extent3D {
                width: scene.extent.width,
                height: scene.extent.height,
                depth: 1,
            })
            .mip_levels(1)
            .array_layers(1)
            .samples(vk::SampleCountFlags::TYPE_1)
            .tiling(vk::ImageTiling::OPTIMAL)
            .usage(vk::ImageUsageFlags::COLOR_ATTACHMENT | vk::ImageUsageFlags::SAMPLED)
            .sharing_mode(vk::SharingMode::EXCLUSIVE)
            .initial_layout(vk::ImageLayout::UNDEFINED);
        target.image = unsafe { self.runtime.device.create_image(&image_info, None) }
            .context("create transition image")?;
        let requirements = unsafe {
            self.runtime
                .device
                .get_image_memory_requirements(target.image)
        };
        let memory_type = self
            .find_memory_type(
                requirements.memory_type_bits,
                vk::MemoryPropertyFlags::DEVICE_LOCAL,
            )
            .ok_or_else(|| anyhow!("no device-local memory for transition image"))?;
        let allocate = vk::MemoryAllocateInfo::default()
            .allocation_size(requirements.size)
            .memory_type_index(memory_type);
        target.memory = unsafe { self.runtime.device.allocate_memory(&allocate, None) }
            .context("allocate transition image")?;
        unsafe {
            self.runtime
                .device
                .bind_image_memory(target.image, target.memory, 0)
        }
        .context("bind transition image")?;
        let view_info = vk::ImageViewCreateInfo::default()
            .image(target.image)
            .view_type(vk::ImageViewType::TYPE_2D)
            .format(scene.format)
            .subresource_range(full_color_range());
        target.view = unsafe { self.runtime.device.create_image_view(&view_info, None) }
            .context("create transition image view")?;
        let attachments = [target.view];
        let framebuffer_info = vk::FramebufferCreateInfo::default()
            .render_pass(scene.render_pass)
            .attachments(&attachments)
            .width(scene.extent.width)
            .height(scene.extent.height)
            .layers(1);
        target.framebuffer = unsafe {
            self.runtime
                .device
                .create_framebuffer(&framebuffer_info, None)
        }
        .context("create transition framebuffer")?;
        let set_layouts = [self.descriptor_set_layout];
        let allocate_info = vk::DescriptorSetAllocateInfo::default()
            .descriptor_pool(self.descriptor_pool)
            .set_layouts(&set_layouts);
        target.descriptor_set =
            unsafe { self.runtime.device.allocate_descriptor_sets(&allocate_info) }
                .context("allocate transition descriptor set")?[0];
        let scene_info = [vk::DescriptorImageInfo::default()
            .sampler(self.sampler)
            .image_view(scene.all_levels_view)
            .image_layout(vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL)];
        let outgoing_info = [vk::DescriptorImageInfo::default()
            .sampler(self.sampler)
            .image_view(target.view)
            .image_layout(vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL)];
        let writes = [
            vk::WriteDescriptorSet::default()
                .dst_set(target.descriptor_set)
                .dst_binding(1)
                .descriptor_type(vk::DescriptorType::COMBINED_IMAGE_SAMPLER)
                .image_info(&scene_info),
            vk::WriteDescriptorSet::default()
                .dst_set(target.descriptor_set)
                .dst_binding(2)
                .descriptor_type(vk::DescriptorType::COMBINED_IMAGE_SAMPLER)
                .image_info(&outgoing_info),
        ];
        unsafe { self.runtime.device.update_descriptor_sets(&writes, &[]) };
        Ok(())
    }

    fn destroy_transition_resources(&self, resources: TransitionResources) {
        unsafe {
            let descriptor_sets = resources
                .targets
                .iter()
                .map(|target| target.descriptor_set)
                .filter(|set| *set != vk::DescriptorSet::null())
                .collect::<Vec<_>>();
            if !descriptor_sets.is_empty() {
                if let Err(error) = self
                    .runtime
                    .device
                    .free_descriptor_sets(self.descriptor_pool, &descriptor_sets)
                {
                    log::warn!("free transition descriptor sets failed: {error:?}");
                }
            }
            for pipeline in [
                resources.capture_pipeline,
                resources.capture_scene_pipeline,
                resources.output_pipeline,
            ] {
                if pipeline != vk::Pipeline::null() {
                    self.runtime.device.destroy_pipeline(pipeline, None);
                }
            }
            for target in resources.targets {
                if target.framebuffer != vk::Framebuffer::null() {
                    self.runtime
                        .device
                        .destroy_framebuffer(target.framebuffer, None);
                }
                if target.view != vk::ImageView::null() {
                    self.runtime.device.destroy_image_view(target.view, None);
                }
                if target.image != vk::Image::null() {
                    self.runtime.device.destroy_image(target.image, None);
                }
                if target.memory != vk::DeviceMemory::null() {
                    self.runtime.device.free_memory(target.memory, None);
                }
            }
        }
    }
}
