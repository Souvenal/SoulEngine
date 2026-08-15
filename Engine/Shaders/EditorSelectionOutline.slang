public struct EditorSelectionResources {
    public Texture2D<uint> entityId;
};

public struct EditorSelectionConstants {
    public uint2 selectedPixel;
    public uint  enabled;
};

public ParameterBlock<EditorSelectionResources> g_editorSelection;

struct VSOutput {
    float4 position : SV_Position;
};

[shader("vertex")]
VSOutput vertMain(uint vertexId : SV_VertexID) {
    VSOutput output;
    float2 positions[3] = {
        float2(-1.0, -1.0),
        float2( 3.0, -1.0),
        float2(-1.0,  3.0)
    };
    output.position = float4(positions[vertexId], 0.0, 1.0);
    return output;
}

[shader("fragment")]
float4 fragMain(VSOutput input, uniform EditorSelectionConstants selection) : SV_Target0 {
    if (selection.enabled == 0u)
        discard;

    uint width;
    uint height;
    g_editorSelection.entityId.GetDimensions(width, height);
    uint2 selectedPixel = min(selection.selectedPixel, uint2(width - 1u, height - 1u));
    uint selectedId = g_editorSelection.entityId.Load(int3(selectedPixel, 0));
    if (selectedId == 0xffffffffu)
        discard;

    uint2 pixel = uint2(input.position.xy);
    uint currentId = g_editorSelection.entityId.Load(int3(pixel, 0));
    if (currentId == selectedId)
        discard;

    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            if (x == 0 && y == 0)
                continue;
            int2 neighbor = int2(pixel) + int2(x, y);
            if (neighbor.x < 0 || neighbor.y < 0 || neighbor.x >= int(width) || neighbor.y >= int(height))
                continue;
            if (g_editorSelection.entityId.Load(int3(neighbor, 0)) == selectedId)
                return float4(1.0, 0.35, 0.0, 1.0);
        }
    }

    discard;
    return float4(0.0);
}
