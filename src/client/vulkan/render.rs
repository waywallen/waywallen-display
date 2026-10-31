use super::direct::direct_frame_handles;
use super::effects::{
    blur_mip_extent, blur_weights, composition_push_constants, needs_persistent_scene,
    needs_scene_redraw, retained_transition_sample, transition_output_push_constants,
    transition_push_constants,
};
use super::{
    full_color_range, ActiveTransition, BlankState, Composition, DirectRelease, PresentResult,
    WsiPresenter, BLUR_PUSH_CONSTANT_OFFSET, SWAPCHAIN_ACQUIRE_TIMEOUT_NS,
};
use crate as sys;
use anyhow::{anyhow, Context, Result};
use ash::vk;
use ash::vk::Handle;
use std::time::Instant;

impl WsiPresenter {
    pub fn present(
        &mut self,
        display: Option<*mut sys::waywallen_display_t>,
        composition: &Composition,
        now: Instant,
    ) -> Result<PresentResult> {
        let mut release_pending = self.drain_completed_releases(display)?;
        let blur = self.blur_transition.sample(now);
        let transition = self.sample_transition(now);
        let scene_path = needs_persistent_scene(
            self.pause_blur_available,
            self.pause_presentation,
            &blur,
            self.blur_resources.is_some(),
        ) || (self.supports_transitions()
            && (self.transition_presentation.is_some()
                || self.prepared_transition.is_some()
                || self.active_transition.is_some()
                || self.content_presentation.has_submitted()));
        if !scene_path && self.blur_resources.is_some() {
            if !self.frames_idle()? {
                return Ok(PresentResult::Pending);
            }
            self.destroy_current_blur_resources();
        }
        let mut swapchain_recreated = false;
        if let Some(extent) = self.recreate_extent {
            if !self.frames_idle()? {
                return Ok(PresentResult::Pending);
            }
            self.recreate_extent = None;
            self.recreate_swapchain(extent)?;
            swapchain_recreated = true;
        }

        let blank_pending = self.blank_state == BlankState::Pending;
        let retained_transition =
            retained_transition_sample(self.prepared_transition, transition.running);
        let pending_direct = (!blank_pending)
            .then(|| self.pending_direct_frame.as_ref().map(direct_frame_handles))
            .flatten();
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
        let start_transition = pending_direct.is_some()
            && self.supports_transitions()
            && self.content_presentation.should_transition(
                self.transition_presentation.is_some(),
                self.blur_resources
                    .as_ref()
                    .is_some_and(|resources| resources.base_valid),
            );
        if start_transition && self.transition_resources.is_none() {
            match self.create_transition_resources() {
                Ok(resources) => self.transition_resources = Some(resources),
                Err(error) => {
                    self.transitions_available = false;
                    log::warn!(
                        "wallpaper transitions disabled for {}x{} {:?}: {error:#}",
                        self.extent.width,
                        self.extent.height,
                        self.format
                    );
                }
            }
        }
        let start_transition = start_transition && self.transition_resources.is_some();
        let outgoing_target = self.transition_resources.as_ref().map(|resources| {
            if start_transition {
                (resources.front + 1) % resources.targets.len()
            } else {
                resources.front
            }
        });
        let output_transition = if start_transition {
            self.transition_presentation
                .map(|presentation| (presentation.shape, 0.0))
        } else if let Some(presentation) = self.prepared_transition {
            Some((presentation.shape, 0.0))
        } else {
            transition.running
        };
        let scene_valid = self
            .blur_resources
            .as_ref()
            .is_some_and(|resources| resources.base_valid);
        let scene_redraw = needs_scene_redraw(scene_path, scene_valid, &blur, swapchain_recreated)
            || (scene_path
                && scene_valid
                && (transition.running.is_some()
                    || transition.finished
                    || self.content_presentation.has_submitted()));
        if pending_direct.is_none() && !scene_redraw && !blank_pending {
            return Ok(PresentResult::Presented {
                redraw: release_pending,
            });
        }

        let frame_index = self.frame_cursor;
        let frame = &self.frames[frame_index];
        if !unsafe { self.runtime.device.get_fence_status(frame.fence) }
            .context("query WSI frame fence")?
        {
            log::trace!(
                "WSI frame context busy: surface=0x{:x} frame={} pending_release={:?}",
                self.surface.as_raw(),
                frame_index,
                frame
                    .pending_release
                    .as_ref()
                    .map(|release| (release.buffer_generation, release.seq))
            );
            return Ok(PresentResult::Pending);
        }
        log::trace!(
            "vkAcquireNextImageKHR enter: surface=0x{:x} frame={} direct={:?}",
            self.surface.as_raw(),
            frame_index,
            self.pending_direct_frame
                .as_ref()
                .map(|direct| (direct.buffer_generation, direct.seq))
        );
        let acquired = unsafe {
            self.runtime.swapchain_loader.acquire_next_image(
                self.swapchain,
                SWAPCHAIN_ACQUIRE_TIMEOUT_NS,
                frame.image_available,
                vk::Fence::null(),
            )
        };
        log::trace!(
            "vkAcquireNextImageKHR return: surface=0x{:x} frame={} result={acquired:?}",
            self.surface.as_raw(),
            frame_index
        );
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
        let clear_color = if blank_pending {
            [0.0, 0.0, 0.0, 1.0]
        } else {
            composition.clear
        };
        let clear = [vk::ClearValue {
            color: vk::ClearColorValue {
                float32: clear_color,
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
                if start_transition {
                    // Copy what is on screen before the new frame overwrites the scene.
                    let transitions = self.transition_resources.as_ref().unwrap();
                    let capture_begin = vk::RenderPassBeginInfo::default()
                        .render_pass(resources.render_pass)
                        .framebuffer(transitions.targets[outgoing_target.unwrap()].framebuffer)
                        .render_area(vk::Rect2D {
                            offset: vk::Offset2D { x: 0, y: 0 },
                            extent: resources.extent,
                        })
                        .clear_values(&clear);
                    self.runtime.device.cmd_begin_render_pass(
                        frame.command_buffer,
                        &capture_begin,
                        vk::SubpassContents::INLINE,
                    );
                    self.cmd_set_render_extent(frame.command_buffer, resources.extent);
                    let levels = resources.mip_views.len() as u32;
                    let weights = if resources.pyramid_valid {
                        blur_weights(blur.radius, levels)
                    } else {
                        blur_weights(0.0, levels)
                    };
                    if let Some((shape, progress)) = retained_transition {
                        self.runtime.device.cmd_bind_pipeline(
                            frame.command_buffer,
                            vk::PipelineBindPoint::GRAPHICS,
                            transitions.capture_pipeline,
                        );
                        self.runtime.device.cmd_bind_descriptor_sets(
                            frame.command_buffer,
                            vk::PipelineBindPoint::GRAPHICS,
                            self.pipeline_layout,
                            0,
                            &[transitions.targets[transitions.front].descriptor_set],
                            &[],
                        );
                        let push = transition_output_push_constants(
                            weights,
                            transition_push_constants(shape, progress, resources.extent),
                        );
                        self.runtime.device.cmd_push_constants(
                            frame.command_buffer,
                            self.pipeline_layout,
                            vk::ShaderStageFlags::FRAGMENT,
                            BLUR_PUSH_CONSTANT_OFFSET,
                            std::slice::from_raw_parts(
                                push.as_ptr().cast::<u8>(),
                                std::mem::size_of_val(&push),
                            ),
                        );
                    } else {
                        self.runtime.device.cmd_bind_pipeline(
                            frame.command_buffer,
                            vk::PipelineBindPoint::GRAPHICS,
                            transitions.capture_scene_pipeline,
                        );
                        self.runtime.device.cmd_bind_descriptor_sets(
                            frame.command_buffer,
                            vk::PipelineBindPoint::GRAPHICS,
                            self.pipeline_layout,
                            0,
                            &[resources.mix_descriptor_set],
                            &[],
                        );
                        self.runtime.device.cmd_push_constants(
                            frame.command_buffer,
                            self.pipeline_layout,
                            vk::ShaderStageFlags::FRAGMENT,
                            BLUR_PUSH_CONSTANT_OFFSET,
                            std::slice::from_raw_parts(
                                weights.as_ptr().cast::<u8>(),
                                std::mem::size_of_val(&weights),
                            ),
                        );
                    }
                    self.runtime
                        .device
                        .cmd_draw(frame.command_buffer, 3, 1, 0, 0);
                    self.runtime
                        .device
                        .cmd_end_render_pass(frame.command_buffer);
                }
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
                self.cmd_set_render_extent(frame.command_buffer, self.extent);
                let blur_push = blur_weights(blur.radius, resources.mip_views.len() as u32);
                if let (Some((shape, progress)), Some(transitions)) =
                    (output_transition, self.transition_resources.as_ref())
                {
                    self.runtime.device.cmd_bind_pipeline(
                        frame.command_buffer,
                        vk::PipelineBindPoint::GRAPHICS,
                        transitions.output_pipeline,
                    );
                    self.runtime.device.cmd_bind_descriptor_sets(
                        frame.command_buffer,
                        vk::PipelineBindPoint::GRAPHICS,
                        self.pipeline_layout,
                        0,
                        &[transitions.targets[outgoing_target.unwrap()].descriptor_set],
                        &[],
                    );
                    let push = transition_output_push_constants(
                        blur_push,
                        transition_push_constants(shape, progress, self.extent),
                    );
                    self.runtime.device.cmd_push_constants(
                        frame.command_buffer,
                        self.pipeline_layout,
                        vk::ShaderStageFlags::FRAGMENT,
                        BLUR_PUSH_CONSTANT_OFFSET,
                        std::slice::from_raw_parts(
                            push.as_ptr().cast::<u8>(),
                            std::mem::size_of_val(&push),
                        ),
                    );
                } else {
                    self.runtime.device.cmd_bind_pipeline(
                        frame.command_buffer,
                        vk::PipelineBindPoint::GRAPHICS,
                        resources.blur_pipeline,
                    );
                    self.runtime.device.cmd_bind_descriptor_sets(
                        frame.command_buffer,
                        vk::PipelineBindPoint::GRAPHICS,
                        self.pipeline_layout,
                        0,
                        &[resources.mix_descriptor_set],
                        &[],
                    );
                    self.runtime.device.cmd_push_constants(
                        frame.command_buffer,
                        self.pipeline_layout,
                        vk::ShaderStageFlags::FRAGMENT,
                        BLUR_PUSH_CONSTANT_OFFSET,
                        std::slice::from_raw_parts(
                            blur_push.as_ptr().cast::<u8>(),
                            std::mem::size_of_val(&blur_push),
                        ),
                    );
                }
                self.runtime
                    .device
                    .cmd_draw(frame.command_buffer, 3, 1, 0, 0);
                self.runtime
                    .device
                    .cmd_end_render_pass(frame.command_buffer);
            } else if let Some(push_bytes) = composition_push_bytes {
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
            } else {
                self.runtime.device.cmd_begin_render_pass(
                    frame.command_buffer,
                    &swapchain_begin,
                    vk::SubpassContents::INLINE,
                );
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
        let signal_semaphores = [present_ready];
        let submit = [vk::SubmitInfo::default()
            .wait_semaphores(&wait_semaphores)
            .wait_dst_stage_mask(&wait_stages)
            .command_buffers(&command_buffers)
            .signal_semaphores(&signal_semaphores)];
        log::trace!(
            "vkQueueSubmit enter: surface=0x{:x} frame={} image={} direct={:?} waits={}",
            self.surface.as_raw(),
            frame_index,
            image_index,
            self.pending_direct_frame
                .as_ref()
                .map(|direct| (direct.buffer_generation, direct.seq)),
            wait_semaphores.len()
        );
        unsafe {
            self.runtime
                .device
                .queue_submit(self.runtime.graphics_queue, &submit, frame.fence)
        }
        .context("vkQueueSubmit WSI frame")?;
        log::trace!(
            "vkQueueSubmit return: surface=0x{:x} frame={} image={}",
            self.surface.as_raw(),
            frame_index,
            image_index
        );
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
        if start_transition {
            if let (Some(resources), Some(target)) =
                (self.transition_resources.as_mut(), outgoing_target)
            {
                resources.front = target;
            }
            self.prepared_transition = self.transition_presentation;
            log::debug!(
                "transition prepared: surface=0x{:x} presentation={:?} interrupted={:?}",
                self.surface.as_raw(),
                self.transition_presentation,
                transition.running
            );
        }
        if pending_direct.is_some() {
            self.content_presentation.mark_submitted();
            let direct = self
                .pending_direct_frame
                .take()
                .expect("submitted direct source must remain pending");
            // Ack only after this submission's fence signals. Earlier release lets the
            // producer race the acquire semaphore's pending queue wait.
            self.frames[frame_index].pending_release = Some(DirectRelease {
                release_syncobj_fd: direct.release_syncobj_fd,
                buffer_generation: direct.buffer_generation,
                seq: direct.seq,
            });
            log::trace!(
                "WSI direct release pending: surface=0x{:x} frame={} generation={} seq={}",
                self.surface.as_raw(),
                frame_index,
                direct.buffer_generation,
                direct.seq
            );
            release_pending = true;
        }
        let swapchains = [self.swapchain];
        let image_indices = [image_index];
        let present_wait_semaphores = [present_ready];
        let present_info = vk::PresentInfoKHR::default()
            .wait_semaphores(&present_wait_semaphores)
            .swapchains(&swapchains)
            .image_indices(&image_indices);
        log::trace!(
            "vkQueuePresentKHR enter: surface=0x{:x} frame={} image={} pending_release={:?}",
            self.surface.as_raw(),
            frame_index,
            image_index,
            self.frames[frame_index]
                .pending_release
                .as_ref()
                .map(|release| (release.buffer_generation, release.seq))
        );
        let present = unsafe {
            self.runtime
                .swapchain_loader
                .queue_present(self.runtime.present_queue, &present_info)
        };
        log::trace!(
            "vkQueuePresentKHR return: surface=0x{:x} frame={} image={} result={present:?}",
            self.surface.as_raw(),
            frame_index,
            image_index
        );
        let presented_to_compositor = match present {
            Ok(present_suboptimal) => {
                if suboptimal || present_suboptimal {
                    self.recreate_extent = Some(self.extent);
                }
                true
            }
            Err(vk::Result::ERROR_OUT_OF_DATE_KHR) => {
                self.recreate_extent = Some(self.extent);
                false
            }
            Err(error) => {
                self.content_presentation.discard_submitted();
                self.prepared_transition = None;
                return Err(anyhow!("vkQueuePresentKHR: {error:?}"));
            }
        };
        if presented_to_compositor {
            self.content_presentation.present_accepted();
            if let Some(presentation) = self.prepared_transition.take() {
                self.active_transition = Some(ActiveTransition {
                    presentation,
                    started_at: now,
                });
                log::debug!(
                    "transition started: surface=0x{:x} presentation={presentation:?}",
                    self.surface.as_raw()
                );
            } else if transition.finished {
                self.active_transition = None;
                log::debug!("transition finished: surface=0x{:x}", self.surface.as_raw());
            }
            if blank_pending || pending_direct.is_some() {
                self.blank_state.presented(blank_pending);
            }
        } else if !scene_path {
            self.content_presentation.discard_submitted();
        }
        self.frame_cursor = (self.frame_cursor + 1) % self.frames.len();
        let cleanup_pending =
            !self.pause_presentation.configured && blur.finished && self.blur_resources.is_some();
        Ok(PresentResult::Presented {
            redraw: blur.animating
                || output_transition.is_some()
                || cleanup_pending
                || release_pending
                || self.blank_state == BlankState::Pending
                || self.recreate_extent.is_some()
                || self.pending_direct_frame.is_some(),
        })
    }
}
