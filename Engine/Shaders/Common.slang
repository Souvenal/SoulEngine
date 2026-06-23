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
