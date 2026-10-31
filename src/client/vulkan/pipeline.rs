use super::{
    FrameContext, WsiPresenter, BLUR_PUSH_CONSTANT_OFFSET, BLUR_PUSH_CONSTANT_SIZE,
    COMPOSITION_PUSH_CONSTANT_SIZE, FRAGMENT_SHADER, FRAMES_IN_FLIGHT, MAX_BLUR_MIP_LEVELS,
    TOTAL_PUSH_CONSTANT_SIZE, TRANSITION_PUSH_CONSTANT_SIZE, TRANSITION_TARGET_COUNT,
    VERTEX_SHADER,
};
use anyhow::{anyhow, bail, Context, Result};
use ash::vk;
use std::ffi::CString;

impl WsiPresenter {
    pub(super) fn initialize(&mut self, extent: (u32, u32)) -> Result<()> {
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
            vk::DescriptorSetLayoutBinding::default()
                .binding(2)
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
        let max_sets = FRAMES_IN_FLIGHT + MAX_BLUR_MIP_LEVELS as usize + TRANSITION_TARGET_COUNT;
        let pool_sizes = [vk::DescriptorPoolSize {
            ty: vk::DescriptorType::COMBINED_IMAGE_SAMPLER,
            descriptor_count: (max_sets * descriptor_binding.len()) as u32,
        }];
        let pool_info = vk::DescriptorPoolCreateInfo::default()
            .flags(vk::DescriptorPoolCreateFlags::FREE_DESCRIPTOR_SET)
            .max_sets(max_sets as u32)
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
                .size(BLUR_PUSH_CONSTANT_SIZE + TRANSITION_PUSH_CONSTANT_SIZE),
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
                fence: vk::Fence::null(),
                descriptor_set,
                pending_release: None,
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

    pub(super) fn create_swapchain_rendering(
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

    pub(super) fn cmd_set_render_extent(
        &self,
        command_buffer: vk::CommandBuffer,
        extent: vk::Extent2D,
    ) {
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

    pub(super) fn create_pipeline(
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

    pub(super) fn destroy_swapchain_rendering(&mut self) {
        let views = std::mem::take(&mut self.image_views);
        let framebuffers = std::mem::take(&mut self.framebuffers);
        let render_pass = std::mem::replace(&mut self.render_pass, vk::RenderPass::null());
        let pipeline = std::mem::replace(&mut self.pipeline, vk::Pipeline::null());
        self.destroy_swapchain_rendering_parts(views, render_pass, pipeline, framebuffers);
        self.images.clear();
    }

    pub(super) fn destroy_swapchain_rendering_parts(
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
