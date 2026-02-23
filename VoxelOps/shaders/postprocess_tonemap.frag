#version 330 core
out vec4 FragColor;
in vec2 vNDC;

uniform sampler2D uHdrTex;

void main() {
    vec2 uv = vNDC * 0.5 + 0.5;
    vec3 hdr = texture(uHdrTex, clamp(uv, vec2(0.0), vec2(1.0))).rgb;
    const vec3 whitePoint = vec3(1.08241, 0.96756, 0.95003);
    const float exposure = 0.45;
    vec3 color = vec3(1.0) - exp(-max(hdr, vec3(0.0)) / whitePoint * exposure);
    color = pow(max(color, vec3(0.0)), vec3(1.0 / 2.2));
    FragColor = vec4(color, 1.0);
}
