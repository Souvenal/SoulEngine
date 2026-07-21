[[vk::binding(0, 0)]] RaytracingAccelerationStructure g_tlas;
[[vk::binding(1, 0)]] RWTexture2D<float4> g_output;

struct Payload {
    float3 Color;
};

[shader("raygeneration")]
void rayGenMain() {
    uint2 Pixel = DispatchRaysIndex().xy;
    RayDesc Ray = {};
    Ray.Origin = float3(0.0, 0.0, -1.0);
    Ray.Direction = float3(0.0, 0.0, 1.0);
    Ray.TMin = 0.001;
    Ray.TMax = 1000.0;

    Payload RayPayload;
    RayPayload.Color = float3(0.0, 0.0, 0.0);
    TraceRay(g_tlas, RAY_FLAG_NONE, 0xFF, 0, 0, 0, Ray, RayPayload);
    g_output[Pixel] = float4(RayPayload.Color, 1.0);
}

[shader("miss")]
void missMain(inout Payload RayPayload) {
    RayPayload.Color = float3(0.1, 0.2, 0.3);
}

[shader("closesthit")]
void closestHitMain(inout Payload RayPayload, in BuiltInTriangleIntersectionAttributes Attributes) {
    RayPayload.Color = float3(Attributes.barycentrics, 0.0);
}
