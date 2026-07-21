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

public struct RayTracingGeometryResources {
    public ByteAddressBuffer positions;
    public ByteAddressBuffer normals;
    public ByteAddressBuffer indices;
};
public ParameterBlock<RayTracingGeometryResources> g_rayTracingGeometry;

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
    uint firstIndex = PrimitiveIndex() * 3;
    uint3 indices = uint3(g_rayTracingGeometry.indices.Load(firstIndex * 4),
                          g_rayTracingGeometry.indices.Load((firstIndex + 1) * 4),
                          g_rayTracingGeometry.indices.Load((firstIndex + 2) * 4));
    float3 barycentrics = float3(1.0 - attributes.barycentrics.x - attributes.barycentrics.y,
                                  attributes.barycentrics.x,
                                  attributes.barycentrics.y);
    float3 normal0 = asfloat(g_rayTracingGeometry.normals.Load3(indices.x * 12));
    float3 normal1 = asfloat(g_rayTracingGeometry.normals.Load3(indices.y * 12));
    float3 normal2 = asfloat(g_rayTracingGeometry.normals.Load3(indices.z * 12));
    float3 objectNormal = normalize(normal0 * barycentrics.x + normal1 * barycentrics.y + normal2 * barycentrics.z);
    float3 worldNormal = normalize(mul(objectNormal, (float3x3)ObjectToWorld3x4()));
    float3 lightDirection = normalize(-g_rayTracingView.view.directionalLightDirectionIntensity.xyz);
    float3 radiance = g_rayTracingView.view.directionalLightColor.rgb *
                      g_rayTracingView.view.directionalLightDirectionIntensity.w;
    float nDotL = saturate(dot(worldNormal, lightDirection));
    float3 baseColor = float3(0.62, 0.28, 0.10);
    payload.Color = baseColor * (0.08 + radiance * (nDotL / 3.14159265359));
}
