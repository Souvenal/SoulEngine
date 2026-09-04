import Material;
import GBuffer;
import Common;

public struct InstanceData {
    public float4x4 worldTransform;
    public uint     materialIndex;
    public uint     entityId;
    public uint     geometryId;
};

public struct GeometryRecord {
    public float4  boundingSphere;
    public uint64_t positionAddress;
    public uint64_t normalAddress;
    public uint64_t tangentAddress;
    public uint64_t texCoordAddress;
    public uint64_t indexAddress;
};

public struct RasterFrameViewResources {
    public ConstantBuffer<FrameData>       frame;
    public ConstantBuffer<ViewData>        view;
    public StructuredBuffer<float4>        lights;
};

public ParameterBlock<RasterFrameViewResources> g_rasterFrameView;

public struct RasterDrawResources {
    public StructuredBuffer<InstanceData>     instances;
    public StructuredBuffer<GeometryRecord>   geometries;
    public StructuredBuffer<MaterialRecord>   materials;
};

public ParameterBlock<RasterDrawResources> g_rasterDraw;

struct VSOutput {
    float4 position : SV_Position;
    float3 worldPosition;
    float3 normal;
    float4 tangent;
    float2 uv;
    nointerpolation uint instanceIndex;
};

float getTransformOrientation(float3x3 transform) {
    float determinant = dot(transform[0], cross(transform[1], transform[2]));
    return determinant < 0.0 ? -1.0 : 1.0;
}

float3 transformNormalToWorld(float3 normal, float3x3 worldTransform) {
    float3 cofactor0 = cross(worldTransform[1], worldTransform[2]);
    float3 cofactor1 = cross(worldTransform[2], worldTransform[0]);
    float3 cofactor2 = cross(worldTransform[0], worldTransform[1]);
    float3 transformed = normal.x * cofactor0 + normal.y * cofactor1 + normal.z * cofactor2;
    return normalize(transformed * getTransformOrientation(worldTransform));
}

float3 loadFloat3(uint64_t address) {
    float* values = (float*)address;
    return float3(values[0], values[1], values[2]);
}

uint loadUint32(uint64_t address) {
    return ((uint*)address)[0];
}

[shader("vertex")]
VSOutput vertMain(uint vertexID : SV_VertexID, uint instanceID : SV_InstanceID, uint startInstance : SV_StartInstanceLocation) {
    VSOutput output;
    uint instanceIndex = instanceID + startInstance;
    InstanceData instanceData = g_rasterDraw.instances[instanceIndex];
    GeometryRecord geometry = g_rasterDraw.geometries[instanceData.geometryId];
    uint localVertexIndex = loadUint32(geometry.indexAddress + uint64_t(vertexID) * uint64_t(4));
    float3 localPosition = loadFloat3(geometry.positionAddress + uint64_t(localVertexIndex) * uint64_t(12));
    float3 localNormal = loadFloat3(geometry.normalAddress + uint64_t(localVertexIndex) * uint64_t(12));
    float4 localTangent = float4(1.0, 0.0, 0.0, 1.0);
    if (geometry.tangentAddress != 0)
        localTangent = ((float4*)geometry.tangentAddress)[localVertexIndex];
    float2 localUV = float2(0.0, 0.0);
    if (geometry.texCoordAddress != 0)
        localUV = ((float2*)geometry.texCoordAddress)[localVertexIndex];
    float4 worldPosition = mul(float4(localPosition, 1.0), instanceData.worldTransform);
    float3x3 worldTransform = (float3x3)instanceData.worldTransform;
    output.worldPosition = worldPosition.xyz;
    output.normal = transformNormalToWorld(localNormal, worldTransform);
    output.tangent = float4(
        normalize(mul(localTangent.xyz, worldTransform)),
        localTangent.w * getTransformOrientation(worldTransform));
    output.uv = localUV;
    output.instanceIndex = instanceIndex;
    output.position = mul(worldPosition, g_rasterFrameView.view.viewProjection);
    return output;
}

[shader("fragment")]
GBufferPixel fragMain(VSOutput input) {
    InstanceData instanceData = g_rasterDraw.instances[input.instanceIndex];
    MaterialSurface surface = g_rasterDraw.materials[instanceData.materialIndex].resolveSurface(
        input.uv,
        input.normal,
        input.tangent);
    return GBufferPixel(
        float4(surface.baseColor, 1.0),
        surface.normal,
        instanceData.materialIndex,
        instanceData.entityId);
}
