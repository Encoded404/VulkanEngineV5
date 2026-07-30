[[vk::binding(0, 0)]]
Texture2D<float4> allTextures[];
[[vk::binding(0, 0)]]
SamplerState defaultSampler;

// Camera position (fragment-stage push constant, 16 bytes at offset 0)
[[vk::push_constant]]
float4 camera_position;

// Scene header (set 4, binding 0)
struct SceneHeader {
    float4 ambient_color;
    float4 sun_direction;
    float4 sun_color;
    uint light_count;
};

[[vk::binding(0, 4)]]
StructuredBuffer<SceneHeader> sceneHeader;

// Light array (set 4, binding 1) — BlockArray-backed unbounded array
struct Light {
    float4 position;
    float4 color;
    float4 direction;
    float4 params;
};

[[vk::binding(1, 4)]]
StructuredBuffer<Light> sceneLights[];

// Per-material data (set 5, binding 0)
struct MaterialData {
    uint albedo_texture;
    uint normal_texture;
    uint orm_texture;
    float roughness_factor;
    float metallic_factor;
    float ao_factor;
};

[[vk::binding(0, 5)]]
StructuredBuffer<MaterialData> materialBuffer[];

struct PSInput {
    [[vk::location(0)]] float3 normal : TEXCOORD0;
    [[vk::location(1)]] float2 texCoord : TEXCOORD1;
    [[vk::location(2)]] uint materialId : TEXCOORD2;
    [[vk::location(3)]] float3 worldPos : TEXCOORD3;
};

// Cook-Torrance BRDF

static const float PI = 3.14159265359;

float D_GGX(float NdotH, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

float G_SchlickGGX(float NdotV, float roughness) {
    float a = roughness * roughness;
    float k = a * 0.5;
    return NdotV / (NdotV * (1.0 - k) + k);
}

float G_Smith(float NdotV, float NdotL, float roughness) {
    return G_SchlickGGX(NdotV, roughness) * G_SchlickGGX(NdotL, roughness);
}

float3 F_Schlick(float3 F0, float cosTheta) {
    return F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);
}

[shader("fragment")]
float4 main(PSInput input) : SV_TARGET {
    uint block = input.materialId / 256u;
    uint elem  = input.materialId % 256u;
    MaterialData mat = materialBuffer[NonUniformResourceIndex(block)][elem];

    // Sample textures
    float3 albedo = allTextures[NonUniformResourceIndex(mat.albedo_texture)]
        .Sample(defaultSampler, input.texCoord).rgb;

    float3 orm = float3(mat.ao_factor, mat.roughness_factor, mat.metallic_factor);
    if (mat.orm_texture != 0) {
        orm = allTextures[NonUniformResourceIndex(mat.orm_texture)]
            .Sample(defaultSampler, input.texCoord).rgb;
    }
    float ao = orm.r;
    float roughness = max(orm.g, 0.04);
    float metallic = orm.b;

    // Vectors
    float3 N = normalize(input.normal);
    float3 V = normalize(camera_position.xyz - input.worldPos);

    // Cook-Torrance BRDF
    float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);

    float3 combined_direct = float3(0.0, 0.0, 0.0);
    const uint lc = sceneHeader[0].light_count;

    // Accumulate all lights
    for (uint li = 0; li < lc; li++) {
        uint lblock = li / 256u;
        uint lelem  = li % 256u;
        Light lt = sceneLights[NonUniformResourceIndex(lblock)][lelem];

        float3 L = normalize(-lt.direction.xyz);
        float3 H = normalize(V + L);
        float NdotV = max(dot(N, V), 0.001);
        float NdotL = max(dot(N, L), 0.001);
        float NdotH = max(dot(N, H), 0.001);

        float D = D_GGX(NdotH, roughness);
        float G = G_Smith(NdotV, NdotL, roughness);
        float3 F = F_Schlick(F0, max(dot(H, V), 0.0));

        float3 kD = (1.0 - F) * (1.0 - metallic);
        float3 specular = D * G * F / (4.0 * NdotV * NdotL + 0.0001);
        float3 diffuse = kD * albedo / PI;

        combined_direct += (diffuse + specular) * NdotL
                          * lt.color.rgb * lt.color.w;
    }

    // Ambient
    float3 ambient = albedo * ao * sceneHeader[0].ambient_color.rgb * sceneHeader[0].ambient_color.a;

    float3 color = ambient + combined_direct;
    return float4(color, 1.0);
}
