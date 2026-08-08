#version 450

layout(set = 0, binding = 0) uniform sampler2D frame_texture;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;

void main() {
    color = texture(frame_texture, uv);
}
