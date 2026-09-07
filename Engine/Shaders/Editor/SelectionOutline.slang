module SelectionOutline;

public struct OutlineResources {
    public Texture2D<float4> maskTexture;
    public SamplerState      sampler;
};

public ParameterBlock<OutlineResources> g_outline;

public struct OutlinePushConstants {
    public float2 viewportSize;
};



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
float4 fragMain(VSOutput input, uniform OutlinePushConstants push) : SV_Target0 {
    float2 uv = input.position.xy / push.viewportSize;
    float2 texelSize = 1.0 / push.viewportSize;

    float center = g_outline.maskTexture.Sample(g_outline.sampler, uv).r;

    float edge = 0.0;
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            if (x == 0 && y == 0) continue;
            float2 offset = float2(float(x), float(y)) * texelSize;
            float neighbor = g_outline.maskTexture.Sample(g_outline.sampler, uv + offset).r;
            edge += abs(center - neighbor);
        }
    }

    if (edge <= 0.01) {
        discard;
    }
    return float4(1.0, 0.5, 0.0, 1.0);
}
