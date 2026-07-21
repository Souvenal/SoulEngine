[[vk::binding(0, 0)]] RaytracingAccelerationStructure g_tlas;
[[vk::binding(1, 0)]] RWTexture2D<float4> g_output;

public struct RayTracingViewData {
    public float4x4 viewProjectionInverse;
    public float4 cameraPosition;
};
public struct RayTracingViewResources {
    public ConstantBuffer<RayTracingViewData> view;
};
public ParameterBlock<RayTracingViewResources> g_rayTracingView;

struct Payload { float3 Color; };

[shader("raygeneration")]
void rayGenMain() {
    uint2 pixel = DispatchRaysIndex().xy;
    uint2 size = DispatchRaysDimensions().xy;
    float2 uv = (float2(pixel) + 0.5) / float2(size);
    float4 nearPoint = mul(float4(uv * 2.0 - 1.0, 0.0, 1.0), g_rayTracingView.view.viewProjectionInverse);
    float4 farPoint = mul(float4(uv * 2.0 - 1.0, 1.0, 1.0), g_rayTracingView.view.viewProjectionInverse);
    nearPoint /= nearPoint.w;
    farPoint /= farPoint.w;
    RayDesc ray = {};
    ray.Origin = g_rayTracingView.view.cameraPosition.xyz;
    ray.Direction = normalize(farPoint.xyz - nearPoint.xyz);
    ray.TMin = 0.001;
    ray.TMax = 100.0;
    Payload payload = { float3(0.03, 0.05, 0.10) };
    TraceRay(g_tlas, RAY_FLAG_NONE, 0xFF, 0, 0, 0, ray, payload);
    g_output[pixel] = float4(payload.Color, 1.0);
}

[shader("miss")]
void missMain(inout Payload payload) { payload.Color = float3(0.03, 0.05, 0.10); }

[shader("closesthit")]
void closestHitMain(inout Payload payload, in BuiltInTriangleIntersectionAttributes attributes) {
    payload.Color = float3(attributes.barycentrics, 1.0 - attributes.barycentrics.x - attributes.barycentrics.y);
}
