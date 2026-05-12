#version 330 core
out vec4 FragColor;

struct DirLight {
    vec3 direction;
    vec3 ambient;
    vec3 diffuse;
    vec3 specular;
};

struct PointLight {
    vec3 position;
    vec3 diffuse;
    float constant;
    float linear;
    float quadratic;
    float intensity;  // 0.0 = apagada, 1.0 = encendida
};

in vec3 FragPos;
in vec3 Normal;
in vec2 TexCoords;

uniform vec3      viewPos;
uniform float     shininess;
uniform DirLight  dirLight;
uniform PointLight shotLight;  // luz del disparo

uniform sampler2D texture_diffuse1;

void main()
{
    vec3 color   = texture(texture_diffuse1, TexCoords).rgb;
    vec3 norm    = normalize(Normal);
    vec3 viewDir = normalize(viewPos - FragPos);

    // --- Luz solar (direccional) ---
    vec3 sunDir  = normalize(-dirLight.direction);
    vec3 ambient = dirLight.ambient * color;
    float diff   = max(dot(norm, sunDir), 0.0);
    vec3 diffuse = dirLight.diffuse * diff * color;
    vec3 reflectDir = reflect(-sunDir, norm);
    float spec   = pow(max(dot(viewDir, reflectDir), 0.0), shininess);
    vec3 specular = dirLight.specular * spec * 0.3;
    vec3 result  = ambient + diffuse + specular;

    // --- Luz del disparo (puntual, solo si intensity > 0) ---
    if (shotLight.intensity > 0.0)
    {
        vec3  shotDir     = normalize(shotLight.position - FragPos);
        float distance    = length(shotLight.position - FragPos);
        float attenuation = 1.0 / (shotLight.constant
                          + shotLight.linear    * distance
                          + shotLight.quadratic * distance * distance);

        float shotDiff    = max(dot(norm, shotDir), 0.0);
        vec3  shotDiffuse = shotLight.diffuse * shotDiff * color
                          * attenuation * shotLight.intensity;

        result += shotDiffuse;
    }

    result = pow(result, vec3(1.0/2.2));
    FragColor = vec4(result, 1.0);
}