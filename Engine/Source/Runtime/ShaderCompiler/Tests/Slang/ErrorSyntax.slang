// Missing semicolon
struct BadStruct { float x }

// Undefined type reference
UndefinedType g_bad;

// Mismatched brace — missing closing paren on parameter list
float4 badFunction(float a
{
    return a;
}

// Invalid intrinsic name
float4 test = doesNotExist(1.0);

// Wrong argument count — SamplerState is missing
Texture2D<float4> g_tex;
float4 badSample = g_tex.Sample(float2(0, 0));

struct GoodStruct { float x; };

// Invalid shader stage attribute
[shader("invalid_stage")]
void BadEntryPoint() { }

// Duplicate semantic on struct members
struct DupInput {
    float4 pos  : SV_Position;
    float4 pos2 : SV_Position;
};

// Non-existent attribute
[[DoesNotExist]]
float4 weirdAttribute() { return 0.0; }
