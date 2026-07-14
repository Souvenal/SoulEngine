module Common;

public struct FrameData {
    public float4x4 view;
    public float4x4 projection;
    public float    time;
};

public struct FrameResources {
    public ConstantBuffer<FrameData> cb;
};

public ParameterBlock<FrameResources> g_frame;

// Sampler resources are bound by renderer draw parameters through reflection.
// g_samplers.uSamplerLinear = linear-repeat
// g_samplers.uSamplerAniso  = linear-repeat + max anisotropy
public struct SamplerResources {
    public SamplerState uSamplerLinear;
    public SamplerState uSamplerAniso;
};

public ParameterBlock<SamplerResources> g_samplers;

// Sampled-image array. Runtime array element binding is still tracked in
// RHI/Vulkan/TODO.md; current renderer code writes element 0 by shader binding.
public struct TextureResources {
    public Texture2D<float4> uTextures[];
};

public ParameterBlock<TextureResources> g_textures;
