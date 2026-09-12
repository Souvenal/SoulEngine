module PresentBlit;

// Fullscreen-triangle copy of the ray-tracing output into SceneColorRT.
// Texel-exact Load: no sampler, no filtering.

public struct BlitResources {
    public Texture2D<float4> sourceTexture;
};

public ParameterBlock<BlitResources> g_blit;

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
float4 fragMain(VSOutput input) : SV_Target0 {
    return g_blit.sourceTexture.Load(int3(int2(input.position.xy), 0));
}
