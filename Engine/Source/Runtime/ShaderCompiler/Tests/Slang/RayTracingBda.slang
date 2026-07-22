public struct Resources {
    public RaytracingAccelerationStructure tlas;
    public RWTexture2D<float4> output;
};
public ParameterBlock<Resources> g_resources;

public struct BdaMetadataResources {
    public ByteAddressBuffer metadata;
};
public ParameterBlock<BdaMetadataResources> g_bdaMetadata;

uint64_t loadAddress(uint byteOffset) {
    uint2 words = g_bdaMetadata.metadata.Load2(byteOffset);
    return uint64_t(words.x) | (uint64_t(words.y) << 32);
}

uint loadIndex(uint geometryRecordOffset, uint indexIndex) {
    uint64_t address = loadAddress(geometryRecordOffset + 16) +
                       g_bdaMetadata.metadata.Load(geometryRecordOffset + 32) +
                       uint64_t(indexIndex) * g_bdaMetadata.metadata.Load(geometryRecordOffset + 44);
    uint* source = (uint*)address;
    return source[0];
}

struct Payload { float3 Color; };

[shader("raygeneration")]
void rayGenMain() {
    uint2 pixel = DispatchRaysIndex().xy;
    RayDesc ray = {};
    ray.Origin = float3(0.0, 0.0, -1.0);
    ray.Direction = float3(0.0, 0.0, 1.0);
    ray.TMin = 0.001;
    ray.TMax = 1000.0;
    Payload payload = {float3(0.0, 0.0, 0.0)};
    TraceRay(g_resources.tlas, RAY_FLAG_NONE, 0xFF, 0, 0, 0, ray, payload);
    g_resources.output[pixel] = float4(payload.Color, 1.0);
}

[shader("miss")]
void missMain(inout Payload payload) { payload.Color = float3(0.0, 0.0, 0.0); }

[shader("closesthit")]
void closestHitMain(inout Payload payload, in BuiltInTriangleIntersectionAttributes attributes) {
    uint instanceOffset = g_bdaMetadata.metadata.Load(8);
    uint geometryOffset = g_bdaMetadata.metadata.Load(12);
    uint firstGeometry = g_bdaMetadata.metadata.Load(instanceOffset + InstanceID() * 16);
    uint geometryRecordOffset = geometryOffset + (firstGeometry + GeometryIndex()) * 64;
    uint index = loadIndex(geometryRecordOffset, PrimitiveIndex() * 3);
    payload.Color = float3(index, attributes.barycentrics.x, attributes.barycentrics.y);
}
