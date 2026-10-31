use super::effects::{
    blur_mip_extent, blur_mip_level_count, blur_weights, composition_push_constants,
    ease_in_out_cubic, forward_display, needs_persistent_scene, needs_scene_redraw,
    retained_transition_sample, transition_push_constants,
};
use super::runtime::{choose_queue_families, device_candidate_sort_key};
use super::swapchain::{choose_extent, choose_image_count, choose_surface_format};
use super::*;

fn assert_near(actual: f32, expected: f32) {
    assert!((actual - expected).abs() < 0.001, "{actual} != {expected}");
}

#[test]
fn blank_state_is_committed_once_and_rearmed_by_swapchain_recreate() {
    let mut state = BlankState::Inactive;
    assert!(state.request());
    assert!(!state.request());
    state.presented(true);
    assert_eq!(state, BlankState::Committed);
    assert!(!state.request());
    state.invalidate_swapchain();
    assert_eq!(state, BlankState::Pending);
}

#[test]
fn source_present_replaces_committed_blank() {
    let mut state = BlankState::Committed;
    state.presented(false);
    assert_eq!(state, BlankState::Inactive);
}

#[test]
fn abandoned_blank_does_not_block_the_next_source() {
    let mut state = BlankState::Pending;
    state.abandon();
    assert_eq!(state, BlankState::Inactive);
}

#[test]
fn content_transition_starts_only_after_an_earlier_target_commits() {
    let mut state = ContentPresentationState::default();
    assert!(!state.bind(10));
    assert!(!state.should_transition(true, true));
    state.mark_submitted();
    state.present_accepted();

    assert!(!state.bind(20));
    assert!(state.should_transition(true, true));
    assert!(!state.should_transition(false, true));
    assert!(!state.should_transition(true, false));
    state.mark_submitted();
    state.present_accepted();

    assert!(!state.bind(20));
    assert!(!state.should_transition(true, true));
}

#[test]
fn uncommitted_target_does_not_turn_a_fast_return_into_a_transition() {
    let mut state = ContentPresentationState::default();
    state.bind(10);
    state.mark_submitted();
    state.present_accepted();

    state.bind(20);
    assert!(state.should_transition(true, true));
    state.mark_submitted();
    assert!(state.bind(10));
    assert!(!state.should_transition(true, true));
    state.mark_submitted();
    state.present_accepted();

    state.bind(20);
    assert!(state.should_transition(true, true));
}

#[test]
fn same_content_rebind_keeps_the_committed_identity() {
    let mut state = ContentPresentationState::default();
    state.bind(33);
    state.mark_submitted();
    state.present_accepted();
    state.unbind();
    assert!(!state.bind(33));
    assert!(!state.should_transition(true, true));
}

#[test]
fn rejected_present_does_not_commit_or_suppress_the_retry_transition() {
    let mut state = ContentPresentationState::default();
    state.bind(10);
    state.mark_submitted();
    state.present_accepted();

    state.bind(20);
    assert!(state.should_transition(true, true));
    state.mark_submitted();
    state.discard_submitted();

    assert_eq!(state.committed, Some(10));
    assert!(state.should_transition(true, true));
}

#[test]
fn newer_transition_snapshot_supersedes_or_cancels_prepared_work() {
    let old = TransitionPresentation {
        shape: TransitionShape::Fade,
        duration: Duration::from_millis(300),
    };
    let updated = TransitionPresentation {
        shape: TransitionShape::Wipe { angle: 90 },
        duration: Duration::from_millis(600),
    };
    let mut configured = Some(old);
    let mut prepared = Some(old);
    let mut active = None;

    assert!(!update_transition_snapshot(
        &mut configured,
        &mut prepared,
        &mut active,
        Some(updated),
    ));
    assert_eq!(configured, Some(updated));
    assert_eq!(prepared, Some(updated));
    assert_eq!(
        retained_transition_sample(prepared, Some((TransitionShape::Fade, 0.5))),
        Some((updated.shape, 0.0))
    );

    assert!(update_transition_snapshot(
        &mut configured,
        &mut prepared,
        &mut active,
        None,
    ));
    assert_eq!(configured, None);
    assert_eq!(prepared, None);
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
fn steady_persistent_scene_does_not_redraw_for_release_polling() {
    let idle = BlurSample {
        radius: 0.0,
        animating: false,
        finished: false,
    };
    assert!(!needs_scene_redraw(true, true, &idle, false));

    let animating = BlurSample {
        radius: 0.0,
        animating: true,
        finished: false,
    };
    assert!(needs_scene_redraw(true, true, &animating, false));
    assert!(needs_scene_redraw(true, true, &idle, true));
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

/// Mirrors `reveal()` in transition.frag for one uv sample.
fn reveal(push: [f32; 8], uv: [f32; 2]) -> f32 {
    let [progress, shape, feather, _, a, b, c, d] = push;
    if shape < 0.5 {
        return progress;
    }
    let travelled = if shape < 1.5 {
        uv[0] * a + uv[1] * b + c
    } else {
        ((uv[0] - a) * c).hypot((uv[1] - b) * d)
    };
    let edge = progress * (1.0 + feather);
    let lower = edge - feather;
    let t = ((travelled - lower) / (edge - lower)).clamp(0.0, 1.0);
    1.0 - t * t * (3.0 - 2.0 * t)
}

#[test]
fn transition_easing_is_symmetric_and_pinned_at_the_ends() {
    assert_near(ease_in_out_cubic(0.0), 0.0);
    assert_near(ease_in_out_cubic(0.5), 0.5);
    assert_near(ease_in_out_cubic(1.0), 1.0);
    assert_near(ease_in_out_cubic(0.25) + ease_in_out_cubic(0.75), 1.0);
    assert_near(ease_in_out_cubic(-1.0), 0.0);
    assert_near(ease_in_out_cubic(2.0), 1.0);
}

#[test]
fn transition_shapes_start_fully_outgoing_and_end_fully_incoming() {
    let extent = vk::Extent2D {
        width: 2560,
        height: 1440,
    };
    let shapes = [
        TransitionShape::Fade,
        TransitionShape::Wipe { angle: 0 },
        TransitionShape::Wipe { angle: 135 },
        TransitionShape::Grow { origin: [0.5, 0.5] },
        TransitionShape::Grow { origin: [0.0, 1.0] },
    ];
    let samples = [[0.0, 0.0], [1.0, 0.0], [0.0, 1.0], [1.0, 1.0], [0.3, 0.7]];
    for shape in shapes {
        for uv in samples {
            assert_near(
                reveal(transition_push_constants(shape, 0.0, extent), uv),
                0.0,
            );
            assert_near(
                reveal(transition_push_constants(shape, 1.0, extent), uv),
                1.0,
            );
        }
    }
}

#[test]
fn wipe_angle_sets_the_direction_the_edge_travels() {
    let extent = vk::Extent2D {
        width: 1920,
        height: 1080,
    };
    let halfway = |angle| transition_push_constants(TransitionShape::Wipe { angle }, 0.5, extent);
    assert!(reveal(halfway(0), [0.1, 0.5]) > 0.99);
    assert!(reveal(halfway(0), [0.9, 0.5]) < 0.01);
    assert!(reveal(halfway(90), [0.5, 0.1]) > 0.99);
    assert!(reveal(halfway(90), [0.5, 0.9]) < 0.01);
    assert!(reveal(halfway(180), [0.9, 0.5]) > 0.99);
    assert!(reveal(halfway(270), [0.5, 0.9]) > 0.99);
}

#[test]
fn grow_expands_from_its_origin_in_pixel_space() {
    let extent = vk::Extent2D {
        width: 3840,
        height: 1080,
    };
    let push = transition_push_constants(
        TransitionShape::Grow {
            origin: [0.25, 0.5],
        },
        0.3,
        extent,
    );
    assert!(reveal(push, [0.25, 0.5]) > 0.99);
    assert!(reveal(push, [1.0, 0.0]) < 0.01);
    // Equal pixel distance horizontally and vertically reveals equally.
    let dx = 200.0 / extent.width as f32;
    let dy = 200.0 / extent.height as f32;
    assert_near(
        reveal(push, [0.25 + dx, 0.5]),
        reveal(push, [0.25, 0.5 + dy]),
    );
}

#[test]
fn active_transition_completes_after_its_duration() {
    let now = Instant::now();
    let active = ActiveTransition {
        presentation: TransitionPresentation {
            shape: TransitionShape::Fade,
            duration: Duration::from_millis(400),
        },
        started_at: now,
    };
    assert_near(active.progress_at(now).unwrap(), 0.0);
    assert_near(
        active
            .progress_at(now + Duration::from_millis(200))
            .unwrap(),
        0.5,
    );
    assert!(active
        .progress_at(now + Duration::from_millis(400))
        .is_none());
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
