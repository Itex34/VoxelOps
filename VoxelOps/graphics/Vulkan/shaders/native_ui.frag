#version 450

layout(set = 0, binding = 0) uniform sampler2D u_texture;

layout(push_constant) uniform PushConstants {
    vec2 screen_size;
    uint texture_mode;
} pc;

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;

layout(location = 0) out vec4 out_color;

void main() {
    if (pc.texture_mode == 1u) {
        float alpha = texture(u_texture, v_uv).r;
        out_color = vec4(v_color.rgb, v_color.a * alpha);
    } else if (pc.texture_mode == 2u) {
        out_color = v_color * texture(u_texture, v_uv);
    } else {
        out_color = v_color;
    }
}
