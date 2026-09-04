// Self-contained mirror of the runtime RayTracing.slang program interface:
// same parameter-block resource surface, entry-point set, and BDA metadata
// access pattern.  Shader-compiler tests must not load runtime engine
// shaders, so any runtime-interface regression case belongs in a fixture here.

public struct InstanceData {
    public uint firstGeometry;
    public uint geometryCount;
    public uint entityId;
};

public struct GeometryData {
    public uint64_t indexAddress;
    public uint     positionStride;
};

public struct MaterialData {
    public float4 baseColor;
};

public struct RayTracingResources {
    public RaytracingAccelerationStructure tlas;
    public RWTexture2D<float4>             output;
    public RWTexture2D<float4>             accumulation;
    public StructuredBuffer<MaterialData>  materials;
    public StructuredBuffer<InstanceData>  instances;
    public StructuredBuffer<GeometryData>  geometries;
};

public ParameterBlock<RayTracingResources> g_rayTracing;

struct PathPayload {
    float3 color;
    uint   materialIndex;
};

struct ShadowPayload {
    bool occluded;
};

// Physical-storage-buffer load, mirroring the runtime BDA metadata path.
static uint loadIndex(uint64_t address, uint index) {
    uint* source = (uint*)address;
    return source[index];
}

[shader("raygeneration")]
void rayGenMain() {
    uint2 pixel    = DispatchRaysIndex().xy;
    InstanceData instance = g_rayTracing.instances[0];
    GeometryData geometry = g_rayTracing.geometries[instance.firstGeometry];

    PathPayload payload;
    payload.color         = float3(0.0, 0.0, 0.0);
    payload.materialIndex = loadIndex(geometry.indexAddress, 0);

    RayDesc ray = {};
    ray.Origin    = float3(0.0, 0.0, -1.0);
    ray.Direction = float3(0.0, 0.0, 1.0);
    ray.TMin      = 0.001;
    ray.TMax      = 1000.0;
    TraceRay(g_rayTracing.tlas, RAY_FLAG_NONE, 0xFF, 0, 1, 0, ray, payload);

    float4 accumulated = g_rayTracing.accumulation[pixel];
    g_rayTracing.output[pixel] = float4(payload.color, 1.0) + accumulated;
}

[shader("miss")]
void missMain(inout PathPayload payload) {
    payload.color = float3(0.1, 0.2, 0.3);
}

[shader("miss")]
void shadowMissMain(inout ShadowPayload payload) {
    payload.occluded = false;
}

[shader("closesthit")]
void closestHitMain(inout PathPayload payload, in BuiltInTriangleIntersectionAttributes attribs) {
    MaterialData material = g_rayTracing.materials[payload.materialIndex];
    payload.color = material.baseColor.rgb * float3(attribs.barycentrics, 1.0);
}

[shader("closesthit")]
void shadowClosestHitMain(inout ShadowPayload payload, in BuiltInTriangleIntersectionAttributes attribs) {
    payload.occluded = true;
}
