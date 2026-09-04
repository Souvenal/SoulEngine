import Common.All;
import Common.Material;
import Common.Pbr;

module RayTracing;

static const float kRayEpsilon = 0.001;
static const float kRayMaxDistance = 10000.0;

public struct LightData {
    public float4 colorIntensity;
    public float4 positionRange;
    public float4 directionType;
    public float4 spotCone;
};

public struct RayTracingInstanceData {
    public uint firstGeometry;
    public uint geometryCount;
    public uint entityId;
};

public struct RayTracingGeometryData {
    public uint64_t positionAddress;
    public uint64_t normalAddress;
    public uint64_t tangentAddress;
    public uint64_t texCoordAddress;
    public uint64_t indexAddress;
    public uint positionByteOffset;
    public uint normalByteOffset;
    public uint tangentByteOffset;
    public uint texCoordByteOffset;
    public uint indexByteOffset;
    public uint positionStride;
    public uint normalStride;
    public uint tangentStride;
    public uint texCoordStride;
    public uint indexStride;
    public uint materialIndex;
};

public struct RayTracingFrameData {
    public float time;
    public float exposureEV100;
    public uint  lightCount;
    public float4 pathSettings;
};

public struct RayTracingResources {
    public RaytracingAccelerationStructure tlas;
    public RWTexture2D<float4> output;
    public RWTexture2D<float4> accumulation;
    public RWTexture2D<float4> primaryNormal;
    public RWTexture2D<uint> primaryEntityId;
    public ConstantBuffer<RayTracingFrameData> frame;
    public ConstantBuffer<ViewData>  view;
    public SamplerState              linearSampler;
    public StructuredBuffer<MaterialRecord> materials;
    public StructuredBuffer<LightData> lights;
    public StructuredBuffer<RayTracingInstanceData> instances;
    public StructuredBuffer<RayTracingGeometryData> geometries;
};
public ParameterBlock<RayTracingResources> g_rayTracing;

float3 loadFloat3(uint64_t address) {
    float* values = (float*)address;
    return float3(values[0], values[1], values[2]);
}

uint loadUint32(uint64_t address) {
    uint* value = (uint*)address;
    return value[0];
}

float3 loadPosition(RayTracingGeometryData geometry, uint vertexIndex) {
    uint64_t address = geometry.positionAddress + uint64_t(geometry.positionByteOffset) +
                       uint64_t(vertexIndex) * uint64_t(geometry.positionStride);
    return loadFloat3(address);
}

float3 loadNormal(RayTracingGeometryData geometry, uint vertexIndex) {
    uint64_t address = geometry.normalAddress + uint64_t(geometry.normalByteOffset) +
                       uint64_t(vertexIndex) * uint64_t(geometry.normalStride);
    return loadFloat3(address);
}

float4 loadTangent(RayTracingGeometryData geometry, uint vertexIndex) {
    uint64_t address = geometry.tangentAddress + uint64_t(geometry.tangentByteOffset) +
                       uint64_t(vertexIndex) * uint64_t(geometry.tangentStride);
    float* values = (float*)address;
    return float4(values[0], values[1], values[2], values[3]);
}

float2 loadTexCoord(RayTracingGeometryData geometry, uint vertexIndex) {
    uint64_t address = geometry.texCoordAddress + uint64_t(geometry.texCoordByteOffset) +
                       uint64_t(vertexIndex) * uint64_t(geometry.texCoordStride);
    float* values = (float*)address;
    return float2(values[0], values[1]);
}

uint loadIndex(RayTracingGeometryData geometry, uint indexIndex) {
    uint64_t address = geometry.indexAddress + uint64_t(geometry.indexByteOffset) +
                       uint64_t(indexIndex) * uint64_t(geometry.indexStride);
    return loadUint32(address);
}

struct PathPayload {
    float3 position;
    float3 normal;
    uint materialIndex;
    float2 uv;
    float4 tangent;
    uint hit;
    uint entityId;
};

struct ShadowPayload {
    uint occluded;
};

uint hashUint(uint value) {
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return value;
}

float nextRandom(inout uint state) {
    state = hashUint(state);
    return float(state) * (1.0 / 4294967296.0);
}

bool traceShadow(float3 origin, float3 direction, float maxDistance) {
    RayDesc ray = {};
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = kRayEpsilon;
    ray.TMax = maxDistance;

    ShadowPayload payload = {};
    TraceRay(g_rayTracing.tlas,
             RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH,
             0xFF,
             1,
             0,
             1,
             ray,
             payload);
    return payload.occluded != 0;
}

[shader("raygeneration")]
void rayGenMain() {
    uint2 pixel = DispatchRaysIndex().xy;
    uint2 size = DispatchRaysDimensions().xy;
    uint sampleIndex = uint(g_rayTracing.frame.pathSettings.x);
    uint maxBounces = max(uint(g_rayTracing.frame.pathSettings.y), 1u);
    uint randomState = hashUint(pixel.x + pixel.y * size.x + sampleIndex * 0x9e3779b9u);

    float2 jitter = float2(nextRandom(randomState), nextRandom(randomState));
    float2 uv = (float2(pixel) + jitter) / float2(size);
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 nearPoint = mul(float4(ndc, 0.0, 1.0), g_rayTracing.view.viewProjectionInverse);
    float4 farPoint = mul(float4(ndc, 1.0, 1.0), g_rayTracing.view.viewProjectionInverse);
    nearPoint /= nearPoint.w;
    farPoint /= farPoint.w;

    RayDesc ray = {};
    ray.Origin = g_rayTracing.view.cameraPosition.xyz;
    ray.Direction = normalize(farPoint.xyz - nearPoint.xyz);
    ray.TMin = kRayEpsilon;
    ray.TMax = kRayMaxDistance;

    float3 radiance = float3(0.0, 0.0, 0.0);
    float3 throughput = float3(1.0, 1.0, 1.0);
    const float3 environmentRadiance = float3(0.03, 0.05, 0.10);
    PathPayload primaryPayload = {};

    for (uint bounce = 0; bounce < maxBounces; ++bounce) {
        PathPayload payload = {};
        TraceRay(g_rayTracing.tlas, RAY_FLAG_NONE, 0xFF, 0, 0, 0, ray, payload);
        if (bounce == 0u)
            primaryPayload = payload;
        if (payload.hit == 0) {
            radiance += throughput * environmentRadiance;
            break;
        }

        float3 normal = payload.normal;
        if (dot(normal, -ray.Direction) < 0.0)
            normal = -normal;
        MaterialSurface surface = g_rayTracing.materials[payload.materialIndex].resolveSurface(
            payload.uv, normal, payload.tangent, g_rayTracing.linearSampler);
        normal = surface.normal;
        float3 viewDirection = -ray.Direction;
        radiance += throughput * surface.emissive;
        for (uint lightIndex = 0u; lightIndex < g_rayTracing.frame.lightCount; ++lightIndex) {
            LightData light = g_rayTracing.lights[lightIndex];
            uint lightType = uint(light.directionType.w);
            float3 lightDirection = float3(0.0);
            float3 illuminance = float3(0.0);
            float shadowDistance = kRayMaxDistance;
            if (lightType == 1u) {
                lightDirection = normalize(-light.directionType.xyz);
                illuminance = light.colorIntensity.rgb * light.colorIntensity.w;
            } else {
                float3 toLight = light.positionRange.xyz - payload.position;
                float distanceToLight = length(toLight);
                if (distanceToLight <= kRayEpsilon || distanceToLight >= light.positionRange.w)
                    continue;
                lightDirection = toLight / distanceToLight;
                float rangeFade = saturate(1.0 - pow(distanceToLight / light.positionRange.w, 4.0));
                illuminance = light.colorIntensity.rgb * light.colorIntensity.w * rangeFade * rangeFade /
                              (distanceToLight * distanceToLight);
                shadowDistance = distanceToLight - kRayEpsilon;
                if (lightType == 3u) {
                    float coneCosine = dot(normalize(payload.position - light.positionRange.xyz), normalize(light.directionType.xyz));
                    illuminance *= smoothstep(light.spotCone.y, light.spotCone.x, coneCosine);
                }
            }
            float nDotL = saturate(dot(normal, lightDirection));
            if (nDotL > 0.0 && !traceShadow(payload.position + normal * kRayEpsilon, lightDirection, shadowDistance))
            radiance += throughput * evaluatePbrBrdf(surface, normal, viewDirection, lightDirection) * illuminance * nDotL * surface.occlusion;
        }
        PbrBsdfSample sample = samplePbrBsdf(surface,
                                              normal,
                                              viewDirection,
                                              nextRandom(randomState),
                                              float2(nextRandom(randomState), nextRandom(randomState)));
        float nDotSample = saturate(dot(normal, sample.direction));
        if (sample.pdf <= 0.0 || nDotSample <= 0.0)
            break;
        throughput *= sample.value * (nDotSample / sample.pdf);

        if (bounce >= 2u) {
            float survivalProbability = clamp(max(throughput.x, max(throughput.y, throughput.z)), 0.05, 0.95);
            if (nextRandom(randomState) > survivalProbability)
                break;
            throughput /= survivalProbability;
        }

        ray.Origin = payload.position + normal * kRayEpsilon;
        ray.Direction = sample.direction;
        ray.TMin = kRayEpsilon;
        ray.TMax = kRayMaxDistance;
    }

    float3 accumulated = sampleIndex == 0u
        ? radiance
        : (g_rayTracing.accumulation[pixel].xyz * float(sampleIndex) + radiance) / float(sampleIndex + 1u);
    g_rayTracing.accumulation[pixel] = float4(accumulated, 1.0);
    if (sampleIndex == 0u) {
        if (primaryPayload.hit != 0u) {
            g_rayTracing.primaryNormal[pixel] = float4(normalize(primaryPayload.normal), 1.0);
            g_rayTracing.primaryEntityId[pixel] = primaryPayload.entityId;
        } else {
            g_rayTracing.primaryNormal[pixel] = float4(0.0, 0.0, 1.0, 1.0);
            g_rayTracing.primaryEntityId[pixel] = 0xffffffffu;
        }
    }
    accumulated *= exp2(-g_rayTracing.frame.exposureEV100);
    float3 displayColor = accumulated / (accumulated + 1.0);
    displayColor = pow(displayColor, 1.0 / 2.2);
    g_rayTracing.output[pixel] = float4(displayColor, 1.0);
}

[shader("miss")]
void missMain(inout PathPayload payload) {
    payload.hit = 0;
}

[shader("miss")]
void shadowMissMain(inout ShadowPayload payload) {
    payload.occluded = 0;
}

[shader("closesthit")]
void closestHitMain(inout PathPayload payload, in BuiltInTriangleIntersectionAttributes attributes) {
    RayTracingInstanceData instanceData = g_rayTracing.instances[InstanceID()];
    if (GeometryIndex() >= instanceData.geometryCount) {
        payload.hit = 0;
        return;
    }

    RayTracingGeometryData geometry = g_rayTracing.geometries[instanceData.firstGeometry + GeometryIndex()];
    uint firstIndex = PrimitiveIndex() * 3;
    uint3 indices = uint3(loadIndex(geometry, firstIndex),
                          loadIndex(geometry, firstIndex + 1),
                          loadIndex(geometry, firstIndex + 2));
    float3 barycentrics = float3(1.0 - attributes.barycentrics.x - attributes.barycentrics.y,
                                  attributes.barycentrics.x,
                                  attributes.barycentrics.y);
    float3 position0 = loadPosition(geometry, indices.x);
    float3 position1 = loadPosition(geometry, indices.y);
    float3 position2 = loadPosition(geometry, indices.z);
    float3 normal0 = loadNormal(geometry, indices.x);
    float3 normal1 = loadNormal(geometry, indices.y);
    float3 normal2 = loadNormal(geometry, indices.z);
    float2 uv0 = loadTexCoord(geometry, indices.x);
    float2 uv1 = loadTexCoord(geometry, indices.y);
    float2 uv2 = loadTexCoord(geometry, indices.z);
    float4 objectTangent = float4(1.0, 0.0, 0.0, 1.0);
    if (hasTexture(g_rayTracing.materials[geometry.materialIndex].normalTexture)) {
        float4 tangent0 = loadTangent(geometry, indices.x);
        float4 tangent1 = loadTangent(geometry, indices.y);
        float4 tangent2 = loadTangent(geometry, indices.z);
        objectTangent = tangent0 * barycentrics.x + tangent1 * barycentrics.y + tangent2 * barycentrics.z;
    }
    float3 objectPosition = position0 * barycentrics.x + position1 * barycentrics.y + position2 * barycentrics.z;
    float3 geometricNormal = normalize(cross(position1 - position0, position2 - position0));
    float3 objectNormal = normalize(normal0 * barycentrics.x + normal1 * barycentrics.y + normal2 * barycentrics.z);
    if (dot(objectNormal, geometricNormal) < 0.0)
        objectNormal = -objectNormal;
    payload.position = mul(ObjectToWorld3x4(), float4(objectPosition, 1.0));
    payload.normal = normalize(mul(objectNormal, (float3x3)ObjectToWorld3x4()));
    payload.tangent = float4(normalize(mul(objectTangent.xyz, (float3x3)ObjectToWorld3x4())), objectTangent.w);
    payload.materialIndex = geometry.materialIndex;
    payload.entityId = instanceData.entityId;
    payload.uv = uv0 * barycentrics.x + uv1 * barycentrics.y + uv2 * barycentrics.z;
    payload.hit = 1;
}

[shader("closesthit")]
void shadowClosestHitMain(inout ShadowPayload payload, in BuiltInTriangleIntersectionAttributes attributes) {
    payload.occluded = 1;
}
