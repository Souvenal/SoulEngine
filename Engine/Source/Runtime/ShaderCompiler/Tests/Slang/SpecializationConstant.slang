[SpecializationConstant]
const bool IS_INPUT_ENVIRONMENT_MAP = true;

[SpecializationConstant]
[vk::constant_id(7)]
const uint SAMPLE_COUNT = 16;

[SpecializationConstant]
const float BLEND_FACTOR = 0.5f;

struct OutputBlock
{
    float value;
};

RWStructuredBuffer<OutputBlock> outputBuffer;

[numthreads(1, 1, 1)]
void ComputeMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    // The constants feed a real store, so Slang cannot fold them away.
    outputBuffer[0].value =
        IS_INPUT_ENVIRONMENT_MAP ? float(SAMPLE_COUNT) * BLEND_FACTOR : 0.0f;
}
