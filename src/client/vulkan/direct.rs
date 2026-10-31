use super::{
    full_color_range, DirectBinding, DirectFrame, DirectRelease, WsiPresenter,
    RESOURCE_RETIRE_TIMEOUT_NS,
};
use crate as sys;
use anyhow::{anyhow, bail, Context, Result};
use ash::vk;
use ash::vk::Handle;

impl WsiPresenter {
    pub fn has_direct_binding(&self) -> bool {
        self.direct_binding.is_some()
    }

    pub fn install_direct_binding(
        &mut self,
        generation: u64,
        content_token: u64,
        presentation_config_generation: u64,
        extent: vk::Extent2D,
        images: &[vk::Image],
    ) -> Result<()> {
        if self.direct_binding.is_some() {
            bail!("replace direct binding before retiring its image views");
        }
        if content_token == 0 {
            bail!("direct binding has a zero content token");
        }
        if presentation_config_generation != self.presentation_config_generation {
            bail!(
                "direct binding references presentation config {}, current is {}",
                presentation_config_generation,
                self.presentation_config_generation
            );
        }
        self.direct_binding = Some(DirectBinding {
            generation,
            content_token,
            presentation_config_generation,
            images: images.to_vec(),
            views: vec![vk::ImageView::null(); images.len()],
            format: vk::Format::UNDEFINED,
            extent,
        });
        if self.content_presentation.bind(content_token) {
            self.prepared_transition = None;
            self.active_transition = None;
        }
        Ok(())
    }

    pub fn replace_pending_direct_frame(
        &mut self,
        display: *mut sys::waywallen_display_t,
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
        let incoming = DirectFrame {
            image: binding.images[index],
            view: binding.views[index],
            extent: binding.extent,
            layout: vk::ImageLayout::from_raw(direct.layout as i32),
            external_queue_family: direct.external_queue_family_index,
            acquire_semaphore: vk::Semaphore::from_raw(frame.vk_acquire_semaphore as usize as u64),
            release_syncobj_fd: frame.release_syncobj_fd,
            buffer_generation: frame.buffer_generation,
            seq: frame.seq,
        };
        if let Some(superseded) = self.pending_direct_frame.take() {
            log::trace!(
                "WSI direct frame superseded: surface=0x{:x} generation={} seq={} by generation={} seq={}",
                self.surface.as_raw(),
                superseded.buffer_generation,
                superseded.seq,
                incoming.buffer_generation,
                incoming.seq
            );
            resolve_direct_release(
                Some(display),
                DirectRelease {
                    release_syncobj_fd: superseded.release_syncobj_fd,
                    buffer_generation: superseded.buffer_generation,
                    seq: superseded.seq,
                },
            )?;
        }
        self.pending_direct_frame = Some(incoming);
        log::trace!(
            "WSI direct frame pending: surface=0x{:x} generation={} seq={} buffer={}",
            self.surface.as_raw(),
            frame.buffer_generation,
            frame.seq,
            frame.buffer_index
        );
        Ok(())
    }

    pub fn discard_pending_direct_frame(
        &mut self,
        display: Option<*mut sys::waywallen_display_t>,
    ) -> Result<()> {
        let Some(frame) = self.pending_direct_frame.take() else {
            return Ok(());
        };
        resolve_direct_release(
            display,
            DirectRelease {
                release_syncobj_fd: frame.release_syncobj_fd,
                buffer_generation: frame.buffer_generation,
                seq: frame.seq,
            },
        )
    }

    pub fn retire_direct_binding(
        &mut self,
        display: Option<*mut sys::waywallen_display_t>,
    ) -> Result<()> {
        let discard_result = self.discard_pending_direct_frame(display);
        if let Err(error) = self.wait_frames_idle() {
            log::warn!("timed Vulkan retirement failed, waiting for the shared device: {error:#}");
            unsafe { self.runtime.device.device_wait_idle() }
                .context("wait for shared Vulkan device before retiring direct binding")?;
        }
        let release_result = self.drain_completed_releases(display);
        if let Some(binding) = self.direct_binding.take() {
            log::trace!(
                "retire direct binding: generation={} content_token={} presentation_config={}",
                binding.generation,
                binding.content_token,
                binding.presentation_config_generation
            );
            unsafe {
                for view in binding.views {
                    if view != vk::ImageView::null() {
                        self.runtime.device.destroy_image_view(view, None);
                    }
                }
            }
        }
        self.content_presentation.unbind();
        discard_result?;
        release_result?;
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

    pub(super) fn drain_completed_releases(
        &mut self,
        display: Option<*mut sys::waywallen_display_t>,
    ) -> Result<bool> {
        let mut first_error = None;
        for (frame_index, frame) in self.frames.iter_mut().enumerate() {
            if frame.pending_release.is_none() {
                continue;
            }
            let ready = unsafe { self.runtime.device.get_fence_status(frame.fence) }
                .context("query direct frame release fence")?;
            let release = frame.pending_release.as_ref().unwrap();
            log::trace!(
                "WSI direct release fence: surface=0x{:x} frame={} generation={} seq={} ready={}",
                self.surface.as_raw(),
                frame_index,
                release.buffer_generation,
                release.seq,
                ready
            );
            if ready {
                let release = frame.pending_release.take().unwrap();
                let generation = release.buffer_generation;
                let seq = release.seq;
                if let Err(error) = resolve_direct_release(display, release) {
                    if first_error.is_none() {
                        first_error = Some(error);
                    }
                } else {
                    log::trace!(
                        "WSI direct release resolved: surface=0x{:x} frame={} generation={} seq={}",
                        self.surface.as_raw(),
                        frame_index,
                        generation,
                        seq
                    );
                }
            }
        }
        if let Some(error) = first_error {
            return Err(error);
        }
        Ok(self
            .frames
            .iter()
            .any(|frame| frame.pending_release.is_some()))
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
}

pub(super) type DirectFrameHandles = (
    vk::Image,
    vk::ImageView,
    vk::Extent2D,
    vk::ImageLayout,
    u32,
    vk::Semaphore,
);

pub(super) fn direct_frame_handles(frame: &DirectFrame) -> DirectFrameHandles {
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
    display: Option<*mut sys::waywallen_display_t>,
    release: DirectRelease,
) -> Result<()> {
    let rc = unsafe { sys::waywallen_display_signal_release_syncobj(release.release_syncobj_fd) };
    if rc != sys::WAYWALLEN_OK {
        if display.is_none() {
            log::warn!(
                "signal disconnected direct frame release generation={} seq={} failed: {rc}",
                release.buffer_generation,
                release.seq
            );
            return Ok(());
        }
        bail!(
            "signal direct frame release generation={} seq={} failed: {rc}",
            release.buffer_generation,
            release.seq
        );
    }
    if let Some(display) = display {
        let rc = unsafe {
            sys::waywallen_display_frame_release_armed(
                display,
                release.buffer_generation,
                release.seq,
            )
        };
        if rc != sys::WAYWALLEN_OK {
            bail!(
                "acknowledge direct frame release generation={} seq={} failed: {rc}",
                release.buffer_generation,
                release.seq
            );
        }
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
        Some(display),
        DirectRelease {
            release_syncobj_fd: frame.release_syncobj_fd,
            buffer_generation: frame.buffer_generation,
            seq: frame.seq,
        },
    )
}
