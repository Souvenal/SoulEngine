module;

#include <entt/entt.hpp>
#include <cstddef>
#include <hlsl++.h>

// magic_enum::case_insensitive is not exported in v0.9.8.
// Until the fix lands in a released version, use the header directly here.
#include <magic_enum/magic_enum.hpp>

export module Scene:Light;

import std;
export import Core;

export namespace SoulEngine {

enum class LightType : Uint32 {
    Unknown = 0,
    Directional,
    Point,
};

/// @brief Physically authored light attached to a spatial Scene Entity.
struct LightComponent {
    LightType Type                  = LightType::Unknown;
    Float32   ColorR                = 1.0f;
    Float32   ColorG                = 1.0f;
    Float32   ColorB                = 1.0f;
    Float32   Intensity             = 0.0f;
    Float32   RangeMeters           = 10.0f;

    // Enum field bridges to the YAML string vocabulary so generic meta assignment works.
    // Unknown strings keep the current value and log a warning; LightSystem skips
    // components whose Type stays LightType::Unknown, so they remain inert.
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
struct LightRecord {
    entt::entity    EntityId        = entt::null;
    LightType      Type            = LightType::Unknown;
    hlslpp::float3 Color           = hlslpp::float3(1.0f, 1.0f, 1.0f);
    Float32        Intensity       = 0.0f;
    hlslpp::float3 Position        = hlslpp::float3(0.0f, 0.0f, 0.0f);
    Float32        RangeMeters     = 0.0f;
    hlslpp::float3 Direction       = hlslpp::float3(0.0f, 0.0f, -1.0f);

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
            .ColorIntensity = hlslpp::interop::float4{
                hlslpp::float4{Color.x, Color.y, Color.z, Intensity}},
            .PositionRange = hlslpp::interop::float4{
                hlslpp::float4{Position.x, Position.y, Position.z, RangeMeters}},
            .DirectionType = hlslpp::interop::float4{
                hlslpp::float4{Direction.x, Direction.y, Direction.z, static_cast<Float32>(Type)}},
        };
    }
};

/// @brief System for collecting light data from entities.
///
/// Collects LightComponent + SceneNode pairs into LightRecord records for rendering.
class LightSystem : public ISystem {
  public:
    explicit LightSystem(entt::registry& Registry) : ISystem(Registry) {}
    ~LightSystem() override = default;

    /// @brief Update all light components (currently no per-frame logic needed).
    /// @param DeltaTime Time elapsed since last frame in seconds.
    auto OnUpdate(Float32 DeltaTime) -> void override {
        // LightSystem doesn't need per-frame updates currently
    }

    /// @brief Collect all valid light snapshots from the registry.
    /// @return Vector of LightRecord for all valid light entities.
    [[nodiscard]] auto CollectLights() const -> std::vector<LightRecord> {
        std::vector<LightRecord> Lights;

        const auto LightView = m_Registry.view<LightComponent, TransformComponent>();
        for (const auto Entity : LightView) {
            const auto& Light     = LightView.get<LightComponent>(Entity);
            const auto& Transform = LightView.get<TransformComponent>(Entity);

            const auto WorldForward = hlslpp::mul(hlslpp::float4(0.0f, 0.0f, -1.0f, 0.0f), Transform.WorldTransform);
            const auto Direction    = hlslpp::normalize(hlslpp::float3(WorldForward.x, WorldForward.y, WorldForward.z));
            if (Light.Type != LightType::Directional && Light.Type != LightType::Point)
                continue;

            Lights.emplace_back(LightRecord{
                .EntityId  = Entity,
                .Type      = Light.Type,
                .Color     = hlslpp::float3(Light.ColorR, Light.ColorG, Light.ColorB),
                .Intensity = Light.Intensity,
                .Position  = hlslpp::float3(
                    Transform.WorldTransform[3].x, Transform.WorldTransform[3].y, Transform.WorldTransform[3].z),
                .RangeMeters     = Light.RangeMeters,
                .Direction       = Direction,
            });
        }

        return Lights;
    }
};

} // namespace SoulEngine

namespace SoulEngine {

// Note: this namespace must stay named. In a named module, clang 23 silently
// drops the dynamic initializer of an unreferenced anonymous-namespace variable,
// which would skip this entt meta registration at program startup.
namespace MetaRegistration {

using namespace entt::literals;

struct LightComponentMetaRegistration {
    LightComponentMetaRegistration() {
        entt::meta_factory<LightComponent>{"light"_hs}
            .data<&LightComponent::SetType, &LightComponent::GetType>("type"_hs)
            .data<&LightComponent::ColorR>("color_r"_hs)
            .data<&LightComponent::ColorG>("color_g"_hs)
            .data<&LightComponent::ColorB>("color_b"_hs)
            .data<&LightComponent::Intensity>("intensity"_hs)
            .data<&LightComponent::RangeMeters>("range_meters"_hs)
            .func<&EmplaceComponent<LightComponent>>("emplace"_hs);
    }
};

LightComponentMetaRegistration g_LightComponentMetaRegistration = {};

} // namespace MetaRegistration

} // namespace SoulEngine
