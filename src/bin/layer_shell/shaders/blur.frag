#version 450

layout(set = 0, binding = 1) uniform sampler2D scene_texture;

layout(push_constant) uniform PushConstants {
    layout(offset = 48) vec4 weights_a;
    vec4 weights_b;
} push_constants;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;

void main() {
    color = textureLod(scene_texture, uv, 0.0) * push_constants.weights_a.x;
    color += textureLod(scene_texture, uv, 1.0) * push_constants.weights_a.y;
    color += textureLod(scene_texture, uv, 2.0) * push_constants.weights_a.z;
    color += textureLod(scene_texture, uv, 3.0) * push_constants.weights_a.w;
    color += textureLod(scene_texture, uv, 4.0) * push_constants.weights_b.x;
    color += textureLod(scene_texture, uv, 5.0) * push_constants.weights_b.y;
}
