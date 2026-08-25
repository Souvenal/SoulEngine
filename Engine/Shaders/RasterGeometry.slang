import Pbr;
import GBuffer;

public struct RasterFrameData {
    public float time;
    public float exposureEV100;
    public uint  lightCount;
};

public struct RasterViewData {
    public float4x4 viewProjection;
    public float4   cameraPosition;
};

public struct InstanceData {
    public float4x4 worldTransform;
    public uint     materialIndex;
    public uint     entityId;
    public uint     geometryId;
};

public struct GeometryRecord {
    public float3* positionBuffer;
    public float3* normalBuffer;
    public float4* tangentBuffer;
    public float2* texCoordBuffer;
    public uint*   indexBuffer;
    public float4  boundingSphere;
};

public struct RasterFrameViewResources {
    public ConstantBuffer<RasterFrameData> frame;
    public ConstantBuffer<RasterViewData>  view;
    public StructuredBuffer<float4>        lights;
};

public ParameterBlock<RasterFrameViewResources> g_rasterFrameView;

public struct RasterDrawResources {
    public StructuredBuffer<InstanceData>     instances;
    public StructuredBuffer<GeometryRecord>   geometries;
    public StructuredBuffer<PbrMaterialData>  materials;
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

[shader("vertex")]
VSOutput vertMain(uint vertexID : SV_VertexID, uint instanceIndex : SV_InstanceID) {
    VSOutput output;
    InstanceData instanceData = g_rasterDraw.instances[instanceIndex];
    GeometryRecord geometry = g_rasterDraw.geometries[instanceData.geometryId];
    uint localVertexIndex = geometry.indexBuffer[vertexID];
    float3 localPosition = geometry.positionBuffer[localVertexIndex];
    float3 localNormal = geometry.normalBuffer[localVertexIndex];
    float4 localTangent = float4(1.0, 0.0, 0.0, 1.0);
    if ((uint64_t)geometry.tangentBuffer != 0)
        localTangent = geometry.tangentBuffer[localVertexIndex];
    float2 localUV = float2(0.0, 0.0);
    if ((uint64_t)geometry.texCoordBuffer != 0)
        localUV = geometry.texCoordBuffer[localVertexIndex];
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
    PbrSurfaceMaterial surface = resolvePbrMaterial(
        g_rasterDraw.materials[instanceData.materialIndex],
        input.uv,
        input.normal,
        input.tangent);
    return GBufferPixel(
        float4(surface.material.baseColor, 1.0),
        surface.normal,
        instanceData.materialIndex,
        instanceData.entityId);
}
