import Common;

struct VSInput {
    float3 inPosition;
    float3 inColor;
};

struct VSOutput {
    float4 pos : SV_Position;
    float3 color;
};

[shader("vertex")]
VSOutput vertMain(VSInput input) {
    VSOutput output;
    float4 worldPos = float4(input.inPosition, 1.0);
    float4 viewPos  = mul(worldPos, g_frame.cb.view);
    output.pos      = mul(viewPos, g_frame.cb.projection);
    output.color    = input.inColor;
    return output;
}

[shader("fragment")]
float4 fragMain(VSOutput vertIn) : SV_TARGET {
    float pulse   = 0.5 + 0.5 * sin(g_frame.cb.time);
    float3 pulsed = vertIn.color * pulse;
    return float4(pulsed, 1.0);
}
