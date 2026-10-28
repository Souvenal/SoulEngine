import Common.All;
import Common.Material;

module RasterGeometry;

public struct RasterFrameViewResources {
    public ConstantBuffer<FrameData>       frame;
    public ConstantBuffer<ViewData>        view;
    public SamplerState                    linearSampler;
};

public ParameterBlock<RasterFrameViewResources> g_rasterFrameView;

public struct RasterDrawResources {
    public StructuredBuffer<InstanceRecord> instances;
    public StructuredBuffer<MaterialRecord> materials;
};

public ParameterBlock<RasterDrawResources> g_rasterDraw;

public struct RasterVertexPullingResources {
    public GeometryRecordTable geometryTable;
};

public ParameterBlock<RasterVertexPullingResources> g_rasterPulling;

struct VSOutput {
    float4 position : SV_Position;
    float3 worldPosition;
    float3 normal;
    float4 tangent;
    float2 uv;
    nointerpolation uint instanceIndex;
};

VSOutput buildRasterVertexOutput(VertexInfo vtx, uint instanceIndex) {
    VSOutput output;
    InstanceRecord instanceData = g_rasterDraw.instances[instanceIndex];
    float4 worldPosition = mul(float4(vtx.position, 1.0), instanceData.worldTransform);
    float3x3 worldTransform = (float3x3)instanceData.worldTransform;
    output.worldPosition = worldPosition.xyz;
    output.normal = transformNormalToWorld(vtx.normal, worldTransform);
    output.tangent = float4(
        normalize(mul(vtx.tangent.xyz, worldTransform)),
        vtx.tangent.w * getTransformOrientation(worldTransform));
    output.uv = vtx.uv;
    output.instanceIndex = instanceIndex;
    output.position = mul(worldPosition, g_rasterFrameView.view.viewProjection);
    return output;
}

[shader("vertex")]
VSOutput vertMain(uint vertexID : SV_VertexID, uint instanceID : SV_InstanceID, uint startInstance : SV_StartInstanceLocation) {
    uint instanceIndex = instanceID + startInstance;
    InstanceRecord instanceData = g_rasterDraw.instances[instanceIndex];
    VertexInfo vtx = g_rasterPulling.geometryTable.pullVertexAttributes(instanceData.geometryID, vertexID);
    return buildRasterVertexOutput(vtx, instanceIndex);
}

[shader("vertex")]
VSOutput vertVertexInputMain(
    float3 position : POSITION,
    float3 normal : NORMAL,
    float4 tangent : TANGENT,
    float2 uv : TEXCOORD,
    uint instanceID : SV_InstanceID,
    uint startInstance : SV_StartInstanceLocation) {
    VertexInfo vtx;
    vtx.position = position;
    vtx.normal = normal;
    vtx.tangent = tangent;
    vtx.uv = uv;
    return buildRasterVertexOutput(vtx, instanceID + startInstance);
}

[shader("fragment")]
GBufferPixel fragMain(VSOutput input) {
    InstanceRecord instanceData = g_rasterDraw.instances[input.instanceIndex];
    MaterialRecord material = g_rasterDraw.materials[instanceData.materialID];

    // Alpha cutoff
    if (!material.passesAlphaTest(input.uv, g_rasterFrameView.linearSampler))
        discard;

    MaterialSurface surface = material.resolveSurface(
        input.uv,
        input.normal,
        input.tangent,
        g_rasterFrameView.linearSampler);

    return GBufferPixel(
        float4(surface.baseColor, 1.0),
        surface.normal,
        instanceData.materialID,
        instanceData.entityID);
}
