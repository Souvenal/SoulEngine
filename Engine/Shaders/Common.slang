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

// Immutable samplers — Set 1, baked at pipeline layout creation.
// g_samplers.uSamplerLinear = linear-repeat
// g_samplers.uSamplerAniso  = linear-repeat + max anisotropy
public struct SamplerResources {
    public SamplerState uSamplerLinear;
    public SamplerState uSamplerAniso;
};

public ParameterBlock<SamplerResources> g_samplers;

// Bindless sampled-image array — Set 2, variable-count update-after-bind.
// Index selected via push constant (offset 0, size 4).
public struct TextureResources {
    public Texture2D<float4> uTextures[];
};

public ParameterBlock<TextureResources> g_textures;
