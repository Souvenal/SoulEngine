module Common;

public struct FrameData {
    public float time;
};

public struct ViewData {
    public float4x4 viewProjection;
};

public struct FrameViewResources {
    public ConstantBuffer<FrameData> frame;
    public ConstantBuffer<ViewData>  view;
};

public ParameterBlock<FrameViewResources> g_frameView;
