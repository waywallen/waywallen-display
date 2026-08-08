#version 450

layout(push_constant) uniform PushConstants {
    vec4 vertices[6];
} push_constants;

layout(location = 0) out vec2 uv;

void main() {
    vec4 vertex = push_constants.vertices[gl_VertexIndex];
    gl_Position = vec4(vertex.xy, 0.0, 1.0);
    uv = vertex.zw;
}
