#version 450

layout(set = 0, binding = 1) uniform sampler2D source_texture;

layout(push_constant) uniform PushConstants {
    layout(offset = 48) vec4 sample_offset;
    vec4 unused_weights;
} push_constants;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;

void main() {
    vec2 offset = push_constants.sample_offset.xy;
    const float dither = 0.33;
    color = (
        texture(source_texture, uv + vec2(offset.x, offset.y * dither))
        + texture(source_texture, uv + vec2(offset.x * dither, -offset.y))
        + texture(source_texture, uv + vec2(-offset.x * dither, offset.y))
        + texture(source_texture, uv + vec2(-offset.x, -offset.y * dither))
    ) * 0.25;
}
