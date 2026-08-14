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
struct LightSnapshot {
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
