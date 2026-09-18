// Fixture: specialization-constant reflection.  Backend attributes and
// file-scope resources are allowed here (fixture scope); do not copy this
// style into runtime shaders.

[SpecializationConstant]
[vk::constant_id(0)]
const bool kIsInputEnvironmentMap = true;

[SpecializationConstant]
[vk::constant_id(1)]
const int kSeedValue = 7;

[SpecializationConstant]
[vk::constant_id(2)]
const float kScale = 2.0;

RWStructuredBuffer<float> g_output;

[shader("compute")]
[numthreads(1, 1, 1)]
void computeMain(uint3 dispatchThreadID : SV_DispatchThreadID) {
    float Value = kScale;
    if (kIsInputEnvironmentMap) {
        Value += 1.0f;
    }
    g_output[dispatchThreadID.x] = Value + float(kSeedValue);
}
