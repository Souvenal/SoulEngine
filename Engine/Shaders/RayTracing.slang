import Pbr;

static const uint kMaxRayTracingInstances = 64;
static const float kRayEpsilon = 0.001;
static const float kRayMaxDistance = 10000.0;

public struct RayTracingViewData {
    public float4x4 viewProjectionInverse;
    public float4 cameraPosition;
    public float4 directionalLightDirectionIntensity;
    public float4 directionalLightColor;
    public float4 pathSettings;
};

public struct RayTracingMaterialData {
    public float4 baseColorMetallic[kMaxRayTracingInstances];
    public float4 roughnessGeometryBase[kMaxRayTracingInstances];
};

public struct RayTracingResources {
    public RaytracingAccelerationStructure tlas;
    public RWTexture2D<float4> output;
    public RWTexture2D<float4> accumulation;
    public ConstantBuffer<RayTracingViewData> view;
    public ConstantBuffer<RayTracingMaterialData> materials;
};
public ParameterBlock<RayTracingResources> g_rayTracing;

/// Fixed metadata binding. The records stored here contain Vulkan-resolved BDA
/// source-buffer addresses; renderer code never binds per-mesh descriptors.
public struct RayTracingGeometryMetadataResources {
    public ByteAddressBuffer metadata;
};
public ParameterBlock<RayTracingGeometryMetadataResources> g_rayTracingGeometryMetadata;

static const uint kInstanceGeometryRecordSize = 16;
static const uint kGeometryAddressRecordSize = 64;

uint64_t loadAddress(ByteAddressBuffer buffer, uint byteOffset) {
    uint2 words = buffer.Load2(byteOffset);
    return uint64_t(words.x) | (uint64_t(words.y) << 32);
}

float3 loadFloat3(uint64_t address) {
    float* values = (float*)address;
    return float3(values[0], values[1], values[2]);
}

uint loadUint32(uint64_t address) {
    uint* value = (uint*)address;
    return value[0];
}

float3 loadPosition(ByteAddressBuffer metadata, uint geometryOffset, uint vertexIndex) {
    uint64_t address = loadAddress(metadata, geometryOffset) +
                       metadata.Load(geometryOffset + 24) +
                       uint64_t(vertexIndex) * metadata.Load(geometryOffset + 36);
    return loadFloat3(address);
}

float3 loadNormal(ByteAddressBuffer metadata, uint geometryOffset, uint vertexIndex) {
    uint64_t address = loadAddress(metadata, geometryOffset + 8) +
                       metadata.Load(geometryOffset + 28) +
                       uint64_t(vertexIndex) * metadata.Load(geometryOffset + 40);
    return loadFloat3(address);
}

uint loadIndex(ByteAddressBuffer metadata, uint geometryOffset, uint indexIndex) {
    uint64_t address = loadAddress(metadata, geometryOffset + 16) +
                       metadata.Load(geometryOffset + 32) +
                       uint64_t(indexIndex) * metadata.Load(geometryOffset + 44);
    return loadUint32(address);
}

struct PathPayload {
    float3 position;
    float3 normal;
    uint instanceIndex;
    uint hit;
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

PbrMaterial getMaterial(uint instanceIndex) {
    float4 baseColorMetallic = g_rayTracing.materials.baseColorMetallic[instanceIndex];
    float roughness = g_rayTracing.materials.roughnessGeometryBase[instanceIndex].x;
    return makePbrMaterial(baseColorMetallic.xyz, baseColorMetallic.w, roughness);
}

bool traceShadow(float3 origin, float3 direction) {
    RayDesc ray = {};
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = kRayEpsilon;
    ray.TMax = kRayMaxDistance;

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
    uint sampleIndex = uint(g_rayTracing.view.pathSettings.x);
    uint maxBounces = max(uint(g_rayTracing.view.pathSettings.y), 1u);
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
    float3 lightDirection = normalize(-g_rayTracing.view.directionalLightDirectionIntensity.xyz);
    float3 lightRadiance = g_rayTracing.view.directionalLightColor.rgb *
                           g_rayTracing.view.directionalLightDirectionIntensity.w;
    const float3 environmentRadiance = float3(0.03, 0.05, 0.10);

    for (uint bounce = 0; bounce < maxBounces; ++bounce) {
        PathPayload payload = {};
        TraceRay(g_rayTracing.tlas, RAY_FLAG_NONE, 0xFF, 0, 0, 0, ray, payload);
        if (payload.hit == 0) {
            radiance += throughput * environmentRadiance;
            break;
        }

        float3 normal = payload.normal;
        if (dot(normal, -ray.Direction) < 0.0)
            normal = -normal;
        float3 viewDirection = -ray.Direction;
        PbrMaterial material = getMaterial(payload.instanceIndex);
        float nDotL = saturate(dot(normal, lightDirection));
        if (nDotL > 0.0 && !traceShadow(payload.position + normal * kRayEpsilon, lightDirection))
            radiance += throughput * evaluatePbrBrdf(material, normal, viewDirection, lightDirection) * lightRadiance * nDotL;

        PbrBsdfSample sample = samplePbrBsdf(material,
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
    ByteAddressBuffer metadata = g_rayTracingGeometryMetadata.metadata;
    uint instanceOffset = metadata.Load(8);
    uint geometryOffset = metadata.Load(12);
    uint instanceRecordOffset = instanceOffset + InstanceID() * kInstanceGeometryRecordSize;
    uint firstGeometry = metadata.Load(instanceRecordOffset);
    uint geometryCount = metadata.Load(instanceRecordOffset + 4);
    if (GeometryIndex() >= geometryCount) {
        payload.hit = 0;
        return;
    }

    uint geometryRecordOffset = geometryOffset +
                                (firstGeometry + GeometryIndex()) * kGeometryAddressRecordSize;
    uint firstIndex = PrimitiveIndex() * 3;
    uint3 indices = uint3(loadIndex(metadata, geometryRecordOffset, firstIndex),
                          loadIndex(metadata, geometryRecordOffset, firstIndex + 1),
                          loadIndex(metadata, geometryRecordOffset, firstIndex + 2));
    float3 barycentrics = float3(1.0 - attributes.barycentrics.x - attributes.barycentrics.y,
                                  attributes.barycentrics.x,
                                  attributes.barycentrics.y);
    float3 position0 = loadPosition(metadata, geometryRecordOffset, indices.x);
    float3 position1 = loadPosition(metadata, geometryRecordOffset, indices.y);
    float3 position2 = loadPosition(metadata, geometryRecordOffset, indices.z);
    float3 normal0 = loadNormal(metadata, geometryRecordOffset, indices.x);
    float3 normal1 = loadNormal(metadata, geometryRecordOffset, indices.y);
    float3 normal2 = loadNormal(metadata, geometryRecordOffset, indices.z);
    float3 objectPosition = position0 * barycentrics.x + position1 * barycentrics.y + position2 * barycentrics.z;
    float3 geometricNormal = normalize(cross(position1 - position0, position2 - position0));
    float3 objectNormal = normalize(normal0 * barycentrics.x + normal1 * barycentrics.y + normal2 * barycentrics.z);
    if (dot(objectNormal, geometricNormal) < 0.0)
        objectNormal = -objectNormal;
    payload.position = mul(ObjectToWorld3x4(), float4(objectPosition, 1.0));
    payload.normal = normalize(mul(objectNormal, (float3x3)ObjectToWorld3x4()));
    payload.instanceIndex = InstanceID();
    payload.hit = 1;
}

[shader("closesthit")]
void shadowClosestHitMain(inout ShadowPayload payload, in BuiltInTriangleIntersectionAttributes attributes) {
    payload.occluded = 1;
}