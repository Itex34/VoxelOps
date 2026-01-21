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
    float diff = max(dot(norm, -lightDir), 0.0);
    vec3 diffuse = diff * lightColor;

    vec3 color = (ambientColor + diffuse) * texture(diffuseTexture, TexCoords).rgb;
    FragColor = vec4(color, 1.0);
}
