struct MathConstants
{
    static const float kPi = 3.14159265;
    static const float kE  = 2.71828183;
};

interface IMathOp
{
    float apply(float a, float b);
};

struct AddOp : IMathOp
{
    float apply(float a, float b) { return a + b; }
};

struct MulOp : IMathOp
{
    float apply(float a, float b) { return a * b; }
};

float binaryOp<let N : int, let Initial : float>(IMathOp op, float values[N])
{
    float result = Initial;
    for (int i = 0; i < N; ++i)
        result = op.apply(result, values[i]);
    return result;
}

[[vk::binding(0, 0)]] RWStructuredBuffer<float> g_output;

[numthreads(64, 1, 1)]
[shader("compute")]
void ComputeMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    float kValues[4] = { 1.0, 2.0, 3.0, 4.0 };

    AddOp addOp;
    MulOp mulOp;

    float sum = binaryOp<4, 0.0>(addOp, kValues);
    float prod = binaryOp<4, 1.0>(mulOp, kValues);

    g_output[dispatchThreadID.x] = sum * MathConstants::kPi + prod * MathConstants::kE;
}
