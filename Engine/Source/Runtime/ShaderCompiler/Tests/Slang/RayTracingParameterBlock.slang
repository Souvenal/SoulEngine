public struct RayTracingResources {
    public RaytracingAccelerationStructure tlas;
    public RWTexture2D<float4> output;
};
public ParameterBlock<RayTracingResources> g_rayTracingResources;

struct Payload { float3 Color; };

[shader("raygeneration")]
void rayGenMain() {
    uint2 pixel = DispatchRaysIndex().xy;
    RayDesc ray = {};
    ray.Origin = float3(0.0, 0.0, -1.0);
    ray.Direction = float3(0.0, 0.0, 1.0);
    ray.TMin = 0.001;
    ray.TMax = 1000.0;
    Payload payload = { float3(0.0, 0.0, 0.0) };
    TraceRay(g_rayTracingResources.tlas, RAY_FLAG_NONE, 0xFF, 0, 0, 0, ray, payload);
    g_rayTracingResources.output[pixel] = float4(payload.Color, 1.0);
}

[shader("miss")]
void missMain(inout Payload payload) { payload.Color = float3(0.1, 0.2, 0.3); }

[shader("closesthit")]
void closestHitMain(inout Payload payload, in BuiltInTriangleIntersectionAttributes attributes) {
    payload.Color = float3(attributes.barycentrics, 0.0);
}
