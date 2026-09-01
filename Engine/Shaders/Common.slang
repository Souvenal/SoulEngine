module Common;

// Sampler resources are shared by shaders that access sampled textures.
// g_samplers.uSamplerLinear = linear-repeat
// g_samplers.uSamplerAniso  = linear-repeat + max anisotropy
public struct SamplerResources {
    public SamplerState uSamplerLinear;
    public SamplerState uSamplerAniso;
};

public ParameterBlock<SamplerResources> g_samplers;

public struct FrameData {
    public float time;
    public float exposureEV100;
    public uint  lightCount;
};

public struct ViewData {
    public float4x4 viewProjection;
    public float4x4 viewProjectionInverse;
    public float4   cameraPosition;
    public float2   viewportSize;
};
