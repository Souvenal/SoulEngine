module;

#include <entt/entt.hpp>
#include <cstddef>
#include <hlsl++.h>

// magic_enum::case_insensitive is not exported in v0.9.8. Until the fix lands in a released version, use the
// header directly here.
#include <magic_enum/magic_enum.hpp>

export module Scene.Light;

import std;
import Core;
import magic_enum;

namespace SoulEngine {

export enum class LightType : Uint32 {
    Unknown = 0,
    Directional,
    Point,
};

/// @brief Physically authored light attached to a spatial Scene Entity.
export struct LightComponent {
    LightType Type        = LightType::Unknown;
    Float32   ColorR      = 1.0f;
    Float32   ColorG      = 1.0f;
    Float32   ColorB      = 1.0f;
    Float32   Intensity   = 0.0f;
    Float32   RangeMeters = 10.0f;

    // Enum field bridges to the YAML string vocabulary so generic meta assignment works. Unknown strings keep
    // the current value and log a warning; LightSystem skips components whose Type stays LightType::Unknown,
    // so they remain inert.
    [[nodiscard]] auto GetType() const -> String {
        return String(magic_enum::enum_name(Type));
    }

    auto SetType(String Value) -> void {
        if (const auto Parsed = magic_enum::enum_cast<LightType>(Value, magic_enum::case_insensitive)) {
            Type = *Parsed;
            return;
        }
        LogWarning("LightComponent: unknown light type '{}'; keeping LightType::Unknown", Value);
    }
};

/// @brief Immutable world-space light record consumed by renderers.
export struct LightRecord {
    entt::entity   EntityId    = entt::null;
    LightType      Type        = LightType::Unknown;
    hlslpp::float3 Color       = hlslpp::float3(1.0f, 1.0f, 1.0f);
    Float32        Intensity   = 0.0f;
    hlslpp::float3 Position    = hlslpp::float3(0.0f, 0.0f, 0.0f);
    Float32        RangeMeters = 0.0f;
    hlslpp::float3 Direction   = hlslpp::float3(0.0f, 0.0f, -1.0f);

    /// @brief GPU light ABI shared by Raster and RayTracing renderers.
    struct alignas(16) GpuData {
        alignas(16) hlslpp::interop::float4 ColorIntensity = hlslpp::interop::float4{
            hlslpp::float4{1.0f, 1.0f, 1.0f, 0.0f}};
        alignas(16) hlslpp::interop::float4 PositionRange = hlslpp::interop::float4{
            hlslpp::float4{0.0f, 0.0f, 0.0f, 0.0f}};
        alignas(16) hlslpp::interop::float4 DirectionType = hlslpp::interop::float4{
            hlslpp::float4{0.0f, 0.0f, -1.0f, 0.0f}};
    };
    static_assert(sizeof(GpuData) == 48);
    static_assert(offsetof(GpuData, ColorIntensity) == 0);
    static_assert(offsetof(GpuData, PositionRange) == 16);
    static_assert(offsetof(GpuData, DirectionType) == 32);

    [[nodiscard]] auto BuildGpuData() const -> GpuData {
        return GpuData{
            .ColorIntensity = hlslpp::interop::float4{hlslpp::float4{Color.x, Color.y, Color.z, Intensity}},
            .PositionRange  = hlslpp::interop::float4{hlslpp::float4{Position.x, Position.y, Position.z, RangeMeters}},
            .DirectionType  = hlslpp::interop::float4{hlslpp::float4{
                Direction.x, Direction.y, Direction.z, static_cast<Float32>(Type)}},
        };
    }
};

/// @brief System for collecting light data from entities.
///
/// Collects LightComponent + SceneNode pairs into LightRecord records for rendering.
export class LightSystem : public ISystem {
  public:
    explicit LightSystem(entt::registry& Registry) : ISystem(Registry) {}
    ~LightSystem() override = default;

    /// @brief Update all light components (currently no per-frame logic needed).
    /// @param DeltaTime Time elapsed since last frame in seconds.
    auto OnUpdate(Float32 DeltaTime) -> void override;

    /// @brief Collect all valid light snapshots from the registry.
    /// @return Vector of LightRecord for all valid light entities.
    [[nodiscard]] auto CollectLights() const -> std::vector<LightRecord>;
};

} // namespace SoulEngine
