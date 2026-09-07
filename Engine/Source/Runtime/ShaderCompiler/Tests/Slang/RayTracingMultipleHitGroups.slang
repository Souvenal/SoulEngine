// Self-contained fixture for assembling a ray-tracing program with multiple
// miss entries and triangle hit groups.

public struct Resources {
    public RaytracingAccelerationStructure scene;
    public RWTexture2D<float4> output;
};

public ParameterBlock<Resources> resources;

struct PathPayload {
    float3 color;
    uint   materialIndex;
};

[shader("raygeneration")]
void rayGenMain() {
    resources.output[DispatchRaysIndex().xy] = float4(1.0, 0.0, 0.0, 1.0);
}

[shader("miss")]
void missMain(inout PathPayload payload) {
    payload.color = float3(0.1, 0.2, 0.3);
}

[shader("miss")]
void secondaryMissMain(inout PathPayload payload) {
    payload.color = float3(0.3, 0.2, 0.1);
}

[shader("closesthit")]
void closestHitMain(inout PathPayload payload, in BuiltInTriangleIntersectionAttributes attribs) {
    payload.color = float3(attribs.barycentrics, 1.0);
}

[shader("closesthit")]
void secondaryClosestHitMain(inout PathPayload payload, in BuiltInTriangleIntersectionAttributes attribs) {
    payload.color = float3(1.0 - attribs.barycentrics, 1.0);
}
