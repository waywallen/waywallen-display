#version 450

layout(push_constant) uniform PushConstants {
    vec4 position_origin;
    vec4 position_axes;
    vec4 uv_origin_scale;
} push_constants;

layout(location = 0) out vec2 uv;

vec2 unit_corner(int vertex_index) {
    switch (vertex_index) {
    case 0:
        return vec2(0.0, 0.0);
    case 1:
        return vec2(1.0, 0.0);
    case 2:
    case 3:
        return vec2(0.0, 1.0);
    case 4:
        return vec2(1.0, 0.0);
    default:
        return vec2(1.0, 1.0);
    }
}

void main() {
    vec2 corner = unit_corner(gl_VertexIndex);
    vec2 position = push_constants.position_origin.xy
        + corner.x * push_constants.position_axes.xy
        + corner.y * push_constants.position_axes.zw;
    gl_Position = vec4(position, 0.0, 1.0);
    uv = push_constants.uv_origin_scale.xy
        + corner * push_constants.uv_origin_scale.zw;
}
