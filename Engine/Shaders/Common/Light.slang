implementing Common;

/// @brief Shared GPU light record used by raster and ray-tracing lighting.
///
/// The three float4 values are intentionally kept semantic and match the
/// std430 storage-buffer layout produced by the runtime record builder.
public struct LightRecord {
    public float4 colorIntensity;
    public float4 positionRange;
    public float4 directionType;
};

public static const uint kLightTypeUnknown = 0u;
public static const uint kLightTypeDirectional = 1u;
public static const uint kLightTypePoint = 2u;
