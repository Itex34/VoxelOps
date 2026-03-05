#version 330 core

in vec3 FragPos;
in vec3 Normal;
in vec2 TexCoords;

out vec4 FragColor;

uniform sampler2D diffuseTexture;
uniform vec3 lightDir = normalize(vec3(-0.5, -1.0, -0.3));
uniform vec3 lightColor = vec3(1.0);
uniform vec3 ambientColor = vec3(0.3);

void main()
{
    // simple Lambert diffuse
    vec3 norm = normalize(Normal);
    // lightDir is provided as fragment -> light direction (same convention as world shader).
    float diff = max(dot(norm, normalize(lightDir)), 0.0);
    vec3 diffuse = diff * lightColor;

    vec4 texel = texture(diffuseTexture, TexCoords);
    if (texel.a < 0.05) {
        discard;
    }
    vec3 color = (ambientColor + diffuse) * texel.rgb;
    FragColor = vec4(color, texel.a);
}
