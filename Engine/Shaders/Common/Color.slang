implementing Common;

// Rec.709 luma coefficients (ITU-R BT.709). Shared by tonemapping, bloom
// thresholding, and any pass that needs a perceptual brightness estimate.
public float luminance(float3 color) {
    return dot(color, float3(0.2126, 0.7152, 0.0722));
}
