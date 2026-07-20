import Common;

struct VSInput {
    float3 inPosition;
    float3 inNormal;
    float4 inTangent;
    float2 inUV;
};

struct VSOutput {
    float4 pos : SV_Position;
    float3 color;
    float2 uv;
};

[shader("vertex")]
VSOutput vertMain(VSInput input) {
    VSOutput output;
    float4 worldPos = float4(input.inPosition, 1.0);
    output.pos      = mul(worldPos, g_frameView.view.viewProjection);
    output.color    = 0.5 + 0.5 * normalize(input.inNormal + input.inTangent.xyz * 0.001);
    output.uv       = input.inUV;
    return output;
}

[shader("fragment")]
float4 fragMain(VSOutput vertIn, uniform uint baseColorTextureIndex) : SV_TARGET {
    float pulse     = 0.5 + 0.5 * sin(g_frameView.frame.time);
    float4 texColor = g_textures.uTextures[baseColorTextureIndex].Sample(g_samplers.uSamplerLinear, vertIn.uv);
    float3 pulsed   = vertIn.color * pulse * texColor.rgb;
    return float4(pulsed, 1.0);
}
