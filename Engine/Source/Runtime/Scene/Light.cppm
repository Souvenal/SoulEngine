module;

#include <entt/entt.hpp>
#include <hlsl++.h>

export module Scene:Light;

export import Core;

export namespace SoulEngine {

enum class LightType : Uint32 {
    Unknown = 0,
    Directional,
    Point,
    Spot,
};

/// @brief Physically authored light attached to a spatial Scene Entity.
struct LightComponent {
    LightType Type                  = LightType::Unknown;
    Float32   ColorR                = 1.0f;
    Float32   ColorG                = 1.0f;
    Float32   ColorB                = 1.0f;
    Float32   Intensity             = 0.0f;
    Float32   RangeMeters           = 10.0f;
    Float32   InnerConeAngleDegrees = 15.0f;
    Float32   OuterConeAngleDegrees = 25.0f;
    bool      CastsShadows          = false;

    [[nodiscard]] auto GetType() const -> String {
        switch (Type) {
        case LightType::Directional:
            return "directional";
        case LightType::Point:
            return "point";
        case LightType::Spot:
            return "spot";
        case LightType::Unknown:
            break;
        }
        return "unknown";
    }

    auto SetType(String Value) -> void {
        if (Value == "directional")
            Type = LightType::Directional;
        else if (Value == "point")
            Type = LightType::Point;
        else if (Value == "spot")
            Type = LightType::Spot;
        else
            Type = LightType::Unknown;
    }
};

/// @brief Immutable world-space light record consumed by renderers.
struct LightRecord {
    LightType      Type            = LightType::Unknown;
    hlslpp::float3 Color           = hlslpp::float3(1.0f, 1.0f, 1.0f);
    Float32        Intensity       = 0.0f;
    hlslpp::float3 Position        = hlslpp::float3(0.0f, 0.0f, 0.0f);
    Float32        RangeMeters     = 0.0f;
    hlslpp::float3 Direction       = hlslpp::float3(0.0f, 0.0f, -1.0f);
    Float32        InnerConeCosine = 1.0f;
    Float32        OuterConeCosine = 1.0f;
    bool           CastsShadows    = false;
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
            const auto AngleScale   = std::numbers::pi_v<Float32> / 180.0f;

            Lights.emplace_back(LightRecord{
                .Type      = Light.Type,
                .Color     = hlslpp::float3(Light.ColorR, Light.ColorG, Light.ColorB),
                .Intensity = Light.Intensity,
                .Position  = hlslpp::float3(
                    Transform.WorldTransform[3].x, Transform.WorldTransform[3].y, Transform.WorldTransform[3].z),
                .RangeMeters     = Light.RangeMeters,
                .Direction       = Direction,
                .InnerConeCosine = std::cos(Light.InnerConeAngleDegrees * AngleScale),
                .OuterConeCosine = std::cos(Light.OuterConeAngleDegrees * AngleScale),
                .CastsShadows    = Light.CastsShadows,
            });
        }

        return Lights;
    }
};

} // namespace SoulEngine

namespace SoulEngine {

namespace {

struct LightComponentMetaRegistration {
    LightComponentMetaRegistration() {
        entt::meta_factory<LightComponent>{}
            .type("light")
            .data<&LightComponent::SetType, &LightComponent::GetType>("type")
            .data<&LightComponent::ColorR>("color_r")
            .data<&LightComponent::ColorG>("color_g")
            .data<&LightComponent::ColorB>("color_b")
            .data<&LightComponent::Intensity>("intensity")
            .data<&LightComponent::RangeMeters>("range_meters")
            .data<&LightComponent::InnerConeAngleDegrees>("inner_cone_angle_degrees")
            .data<&LightComponent::OuterConeAngleDegrees>("outer_cone_angle_degrees")
            .data<&LightComponent::CastsShadows>("casts_shadows");
    }
};

LightComponentMetaRegistration g_LightComponentMetaRegistration = {};

} // namespace

} // namespace SoulEngine
