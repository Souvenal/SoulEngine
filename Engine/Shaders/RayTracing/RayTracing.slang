import Common.All;
import Common.Material;
import Common.Pbr;

module RayTracing;

static const float kRayEpsilon = 0.001;
static const float kRayMaxDistance = 10000.0;

public struct RayTracingFrameData {
    public float time;
    public float exposureEV100;
    public uint  lightCount;
    public float4 pathSettings;
};

public struct RayTracingResources {
    public RaytracingAccelerationStructure tlas;
    public RWTexture2D<float4> output;
    public RWTexture2D<float4> primaryNormal;
    public RWTexture2D<uint> primaryEntityId;
    public ConstantBuffer<RayTracingFrameData> frame;
    public ConstantBuffer<ViewData>  view;
    public SamplerState              linearSampler;
    public StructuredBuffer<MaterialRecord> materials;
    public StructuredBuffer<LightRecord> lights;
    public StructuredBuffer<InstanceRecord> instances;
    public GeometryRecordTable geometryTable;
};
public ParameterBlock<RayTracingResources> g_rayTracing;

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
    uint maxBounces = max(uint(g_rayTracing.frame.pathSettings.y), 1u);
    uint randomState = hashUint(pixel.x + pixel.y * size.x + asuint(g_rayTracing.frame.time));

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
            LightRecord light = g_rayTracing.lights[lightIndex];
            uint lightType = uint(light.directionType.w);
            float3 lightDirection = float3(0.0);
            float3 illuminance = float3(0.0);
            float shadowDistance = kRayMaxDistance;
            if (lightType == kLightTypeDirectional) {
                lightDirection = normalize(-light.directionType.xyz);
                illuminance = light.colorIntensity.rgb * light.colorIntensity.w;
            } else if (lightType == kLightTypePoint) {
                float3 toLight = light.positionRange.xyz - payload.position;
                float distanceToLight = length(toLight);
                if (distanceToLight <= kRayEpsilon || distanceToLight >= light.positionRange.w)
                    continue;
                lightDirection = toLight / distanceToLight;
                float rangeFade = saturate(1.0 - pow(distanceToLight / light.positionRange.w, 4.0));
                illuminance = light.colorIntensity.rgb * light.colorIntensity.w * rangeFade * rangeFade /
                              (distanceToLight * distanceToLight);
                shadowDistance = distanceToLight - kRayEpsilon;
            } else {
                continue;
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

    if (primaryPayload.hit != 0u) {
        g_rayTracing.primaryNormal[pixel] = float4(normalize(primaryPayload.normal), 1.0);
        g_rayTracing.primaryEntityId[pixel] = primaryPayload.entityId;
    } else {
        g_rayTracing.primaryNormal[pixel] = float4(0.0, 0.0, 1.0, 1.0);
        g_rayTracing.primaryEntityId[pixel] = 0xffffffffu;
    }
    float3 accumulated = radiance;
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
    // TLAS slot order matches the instance table: InstanceIndex() is the
    // zero-based TLAS slot and indexes it directly (InstanceID() would be
    // the custom index, which is never assigned). The hit's geometry and
    // material resolve through the instance record — geometry and material
    // are never bound to each other.
    InstanceRecord instanceData = g_rayTracing.instances[InstanceIndex()];
    uint firstIndex = PrimitiveIndex() * 3;
    VertexInfo vertex0 = g_rayTracing.geometryTable.pullVertex(instanceData.geometryID, firstIndex);
    VertexInfo vertex1 = g_rayTracing.geometryTable.pullVertex(instanceData.geometryID, firstIndex + 1);
    VertexInfo vertex2 = g_rayTracing.geometryTable.pullVertex(instanceData.geometryID, firstIndex + 2);
    float3 barycentrics = float3(1.0 - attributes.barycentrics.x - attributes.barycentrics.y,
                                  attributes.barycentrics.x,
                                  attributes.barycentrics.y);
    float4 objectTangent = vertex0.tangent * barycentrics.x + vertex1.tangent * barycentrics.y +
                           vertex2.tangent * barycentrics.z;
    float3 objectPosition = vertex0.position * barycentrics.x + vertex1.position * barycentrics.y +
                            vertex2.position * barycentrics.z;
    float3 geometricNormal = normalize(cross(vertex1.position - vertex0.position,
                                             vertex2.position - vertex0.position));
    float3 objectNormal = normalize(vertex0.normal * barycentrics.x + vertex1.normal * barycentrics.y +
                                    vertex2.normal * barycentrics.z);
    if (dot(objectNormal, geometricNormal) < 0.0)
        objectNormal = -objectNormal;
    payload.position = mul(ObjectToWorld3x4(), float4(objectPosition, 1.0));
    payload.normal = normalize(mul(objectNormal, (float3x3)ObjectToWorld3x4()));
    payload.tangent = float4(normalize(mul(objectTangent.xyz, (float3x3)ObjectToWorld3x4())), objectTangent.w);
    payload.materialIndex = instanceData.materialID;
    payload.entityId = instanceData.entityID;
    payload.uv = vertex0.uv * barycentrics.x + vertex1.uv * barycentrics.y + vertex2.uv * barycentrics.z;
    payload.hit = 1;
}

[shader("closesthit")]
void shadowClosestHitMain(inout ShadowPayload payload, in BuiltInTriangleIntersectionAttributes attributes) {
    payload.occluded = 1;
}
