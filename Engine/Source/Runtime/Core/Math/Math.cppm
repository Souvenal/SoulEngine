module;

#include <hlsl++.h>

export module Core:Math;

export import :Util.Types;

export namespace SoulEngine {

/// @brief Local three-dimensional translation, rotation, and scale.
struct Transform {
    hlslpp::float3 Translation = hlslpp::float3(0.0f, 0.0f, 0.0f);
    hlslpp::float3 Rotation    = hlslpp::float3(0.0f, 0.0f, 0.0f);
    hlslpp::float3 Scale       = hlslpp::float3(1.0f, 1.0f, 1.0f);

    [[nodiscard]] auto GetLocalMatrix() const -> hlslpp::float4x4 {
        const auto RotationRadians = Rotation * (std::numbers::pi_v<float> / 180.0f);
        return hlslpp::mul(hlslpp::mul(hlslpp::mul(hlslpp::mul(hlslpp::float4x4::scale(Scale),
                                                               hlslpp::float4x4::rotation_x(RotationRadians.x)),
                                                   hlslpp::float4x4::rotation_y(RotationRadians.y)),
                                       hlslpp::float4x4::rotation_z(RotationRadians.z)),
                           hlslpp::float4x4::translation(Translation));
    }
};

} // namespace SoulEngine
