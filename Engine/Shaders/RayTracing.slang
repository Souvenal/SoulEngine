public struct RayTracingResources {
    public RaytracingAccelerationStructure tlas;
    public RWTexture2D<float4> output;
};
public ParameterBlock<RayTracingResources> g_rayTracingResources;

public struct RayTracingViewData {
    public float4x4 viewProjectionInverse;
    public float4 cameraPosition;
    public float4 directionalLightDirectionIntensity;
    public float4 directionalLightColor;
};
public struct RayTracingViewResources {
    public ConstantBuffer<RayTracingViewData> view;
};
public ParameterBlock<RayTracingViewResources> g_rayTracingView;

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

struct Payload { float3 Color; };

[shader("raygeneration")]
void rayGenMain() {
    uint2 pixel = DispatchRaysIndex().xy;
    uint2 size = DispatchRaysDimensions().xy;
    float2 uv = (float2(pixel) + 0.5) / float2(size);
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 nearPoint = mul(float4(ndc, 0.0, 1.0), g_rayTracingView.view.viewProjectionInverse);
    float4 farPoint = mul(float4(ndc, 1.0, 1.0), g_rayTracingView.view.viewProjectionInverse);
    nearPoint /= nearPoint.w;
    farPoint /= farPoint.w;
    RayDesc ray = {};
    ray.Origin = g_rayTracingView.view.cameraPosition.xyz;
    ray.Direction = normalize(farPoint.xyz - nearPoint.xyz);
    ray.TMin = 0.001;
    ray.TMax = 100.0;
    Payload payload = { float3(0.03, 0.05, 0.10) };
    TraceRay(g_rayTracingResources.tlas, RAY_FLAG_NONE, 0xFF, 0, 0, 0, ray, payload);
    g_rayTracingResources.output[pixel] = float4(payload.Color, 1.0);
}

[shader("miss")]
void missMain(inout Payload payload) { payload.Color = float3(0.03, 0.05, 0.10); }

[shader("closesthit")]
void closestHitMain(inout Payload payload, in BuiltInTriangleIntersectionAttributes attributes) {
    ByteAddressBuffer metadata = g_rayTracingGeometryMetadata.metadata;
    uint instanceOffset = metadata.Load(8);
    uint geometryOffset = metadata.Load(12);
    uint instanceRecordOffset = instanceOffset + InstanceID() * kInstanceGeometryRecordSize;
    uint firstGeometry = metadata.Load(instanceRecordOffset);
    uint geometryCount = metadata.Load(instanceRecordOffset + 4);
    if (GeometryIndex() >= geometryCount) {
        payload.Color = float3(1.0, 0.0, 1.0);
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
    float3 geometricNormal = normalize(cross(position1 - position0, position2 - position0));
    float3 objectNormal = normalize(normal0 * barycentrics.x + normal1 * barycentrics.y + normal2 * barycentrics.z);
    if (dot(objectNormal, geometricNormal) < 0.0)
        objectNormal = -objectNormal;
    float3 worldNormal = normalize(mul(objectNormal, (float3x3)ObjectToWorld3x4()));
    float3 lightDirection = normalize(-g_rayTracingView.view.directionalLightDirectionIntensity.xyz);
    float3 radiance = g_rayTracingView.view.directionalLightColor.rgb *
                      g_rayTracingView.view.directionalLightDirectionIntensity.w;
    float nDotL = saturate(dot(worldNormal, lightDirection));
    float3 baseColor = float3(0.62, 0.28, 0.10);
    payload.Color = baseColor * (0.08 + radiance * (nDotL / 3.14159265359));
}
