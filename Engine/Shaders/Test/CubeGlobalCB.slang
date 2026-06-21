// CubeGlobalCB.slang — Simple 3D cube using the engine-global constant buffer.
// Reads view/projection matrices and time from Set 0 Binding 0.

struct GlobalData {
    column_major float4x4 view;
    column_major float4x4 projection;
    float                 time;
};
ParameterBlock<GlobalData> g_global;

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
    float4 viewPos  = mul(g_global.view, worldPos);
    output.pos      = mul(g_global.projection, viewPos);
    output.color    = input.inColor;
    return output;
}

[shader("fragment")]
float4 fragMain(VSOutput vertIn) : SV_TARGET {
    float pulse   = 0.5 + 0.5 * sin(g_global.time);
    float3 pulsed = vertIn.color * pulse;
    return float4(pulsed, 1.0);
}
