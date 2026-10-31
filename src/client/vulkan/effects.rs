use super::{
    ActiveTransition, BlurSample, BlurSegment, BlurTransition, Composition,
    CompositionPushConstants, PausePresentation, TransitionPresentation, TransitionShape,
    BLUR_TRANSITION_DURATION, BLUR_WEIGHT_COUNT, MAX_BLUR_MIP_LEVELS, TRANSITION_EDGE_FEATHER,
};
use ash::vk;
use std::time::{Duration, Instant};

impl PausePresentation {
    pub(super) fn target_radius(self) -> f32 {
        if self.configured && self.active {
            self.radius as f32
        } else {
            0.0
        }
    }
}

impl ActiveTransition {
    /// Eased progress, or `None` once the transition has run its course.
    pub(super) fn progress_at(&self, now: Instant) -> Option<f32> {
        let elapsed = now.saturating_duration_since(self.started_at);
        if elapsed >= self.presentation.duration {
            return None;
        }
        let linear = elapsed.as_secs_f32() / self.presentation.duration.as_secs_f32();
        Some(ease_in_out_cubic(linear))
    }
}

pub(super) fn update_transition_snapshot(
    configured: &mut Option<TransitionPresentation>,
    prepared: &mut Option<TransitionPresentation>,
    active: &mut Option<ActiveTransition>,
    presentation: Option<TransitionPresentation>,
) -> bool {
    *configured = presentation;
    if let Some(presentation) = presentation {
        if prepared.is_some() {
            *prepared = Some(presentation);
        }
        return false;
    }
    let prepared = prepared.take().is_some();
    active.take().is_some() || prepared
}

pub(super) fn retained_transition_sample(
    prepared: Option<TransitionPresentation>,
    running: Option<(TransitionShape, f32)>,
) -> Option<(TransitionShape, f32)> {
    prepared
        .map(|presentation| (presentation.shape, 0.0))
        .or(running)
}

pub(super) fn ease_in_out_cubic(t: f32) -> f32 {
    let t = t.clamp(0.0, 1.0);
    if t < 0.5 {
        4.0 * t * t * t
    } else {
        let inverse = -2.0 * t + 2.0;
        1.0 - inverse * inverse * inverse / 2.0
    }
}

/// Fragment push constants for `transition.frag`: progress, shape and feather,
/// then the shape geometry expressed in uv space for this output extent.
pub(super) fn transition_push_constants(
    shape: TransitionShape,
    progress: f32,
    extent: vk::Extent2D,
) -> [f32; 8] {
    let width = extent.width.max(1) as f32;
    let height = extent.height.max(1) as f32;
    let corners = [[0.0, 0.0], [1.0, 0.0], [0.0, 1.0], [1.0, 1.0]];
    match shape {
        TransitionShape::Fade => [
            progress,
            0.0,
            TRANSITION_EDGE_FEATHER,
            0.0,
            0.0,
            0.0,
            0.0,
            0.0,
        ],
        TransitionShape::Wipe { angle } => {
            let radians = (angle % 360) as f32 * std::f32::consts::PI / 180.0;
            let gradient = [width * radians.cos(), height * radians.sin()];
            let travelled = corners.map(|[u, v]| u * gradient[0] + v * gradient[1]);
            let start = travelled.iter().copied().fold(f32::INFINITY, f32::min);
            let end = travelled.iter().copied().fold(f32::NEG_INFINITY, f32::max);
            let span = (end - start).max(f32::EPSILON);
            [
                progress,
                1.0,
                TRANSITION_EDGE_FEATHER,
                0.0,
                gradient[0] / span,
                gradient[1] / span,
                -start / span,
                0.0,
            ]
        }
        TransitionShape::Grow { origin } => {
            let [x, y] = origin.map(|value| value.clamp(0.0, 1.0));
            let reach = corners
                .iter()
                .map(|[u, v]| ((u - x) * width).hypot((v - y) * height))
                .fold(1.0, f32::max);
            [
                progress,
                2.0,
                TRANSITION_EDGE_FEATHER,
                0.0,
                x,
                y,
                width / reach,
                height / reach,
            ]
        }
    }
}

pub(super) fn needs_persistent_scene(
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

pub(super) fn needs_scene_redraw(
    scene_path: bool,
    scene_valid: bool,
    blur: &BlurSample,
    swapchain_recreated: bool,
) -> bool {
    scene_path && scene_valid && (blur.animating || blur.finished || swapchain_recreated)
}

impl BlurTransition {
    pub(super) fn set_target(&mut self, target: f32, now: Instant, animate: bool) -> bool {
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

    pub(super) fn reset(&mut self) {
        *self = Self::default();
    }

    pub(super) fn sample(&mut self, now: Instant) -> BlurSample {
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

pub(super) fn blur_mip_level_count(extent: vk::Extent2D) -> u32 {
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

pub(super) fn blur_mip_extent(extent: vk::Extent2D, level: u32) -> vk::Extent2D {
    vk::Extent2D {
        width: (extent.width >> level).max(1),
        height: (extent.height >> level).max(1),
    }
}

pub(super) fn blur_weights(radius: f32, mip_levels: u32) -> [f32; 8] {
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

pub(super) fn transition_output_push_constants(
    weights: [f32; 8],
    transition: [f32; 8],
) -> [f32; 16] {
    let mut push = [0.0; 16];
    push[..8].copy_from_slice(&weights);
    push[8..].copy_from_slice(&transition);
    push
}

pub(super) fn composition_push_constants(
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

pub(super) fn forward_display(transform: u32, pre_u: f32, pre_v: f32) -> [f32; 2] {
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
