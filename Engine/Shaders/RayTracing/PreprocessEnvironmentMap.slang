implementing RayTracing;

import Common.All;

// Environment-map preprocessing utilities: per-pixel weight for building
// the PDF pyramid used by presampling. Weight = Rec.709 luminance scaled by
// the texel's relative solid angle (cosine of elevation), clamped to the
// float16 maximum so the pyramid stays encodable.

/// Per-pixel weight for the PDF pyramid: luma * relative solid angle.
public float getPixelWeight(Texture2D<float4> envMap, uint2 position, uint2 sourceSize) {
    float3 color = envMap[position].rgb;
    float luma = max(luminance(color), 0.0);

    // Do not sample invalid colors.
    if (isinf(luma) || isnan(luma))
        return 0.0;

    // Relative solid angle for equirectangular projection: cos(elevation).
    // Absolute value is unnecessary; only relative scale across pixels matters.
    float elevation = ((float(position.y) + 0.5) / float(sourceSize.y) - 0.5) * kPI;
    float relativeSolidAngle = cos(elevation);

    return clamp(luma * relativeSolidAngle, 0.0, kFloat16Max);
}
