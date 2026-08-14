module;

#include <entt/entt.hpp>
#include <hlsl++.h>

export module Scene:Components.Light;

import :Components.Core;

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

[[nodiscard]] auto ValidateLightComponent(const SceneComponentValidationContext&,
                                          entt::registry& Registry,
                                          SceneEntity     Entity,
                                          String&         Error) -> bool {
    const auto* Light = Registry.try_get<LightComponent>(Entity);
    if (!Light) {
        Error = "Light component metadata does not contain LightComponent";
        return false;
    }
    if (Light->Type == LightType::Unknown) {
        Error = "type must be directional, point, or spot";
        return false;
    }
    const auto Luminance = 0.2126f * Light->ColorR + 0.7152f * Light->ColorG + 0.0722f * Light->ColorB;
    if (!std::isfinite(Light->ColorR) || !std::isfinite(Light->ColorG) || !std::isfinite(Light->ColorB) ||
        !std::isfinite(Light->Intensity) || !std::isfinite(Light->RangeMeters) ||
        !std::isfinite(Light->InnerConeAngleDegrees) || !std::isfinite(Light->OuterConeAngleDegrees)) {
        Error = "contains a non-finite value";
        return false;
    }
    if (Light->ColorR < 0.0f || Light->ColorG < 0.0f || Light->ColorB < 0.0f || std::abs(Luminance - 1.0f) > 0.001f) {
        Error = "color must be non-negative linear sRGB with Rec.709 luminance equal to one";
        return false;
    }
    if (Light->Intensity < 0.0f) {
        Error = "intensity must be non-negative";
        return false;
    }
    if (Light->Type != LightType::Directional && Light->RangeMeters <= 0.0f) {
        Error = "range_meters must be greater than zero for point and spot lights";
        return false;
    }
    if (Light->Type == LightType::Spot &&
        (Light->InnerConeAngleDegrees <= 0.0f || Light->InnerConeAngleDegrees > Light->OuterConeAngleDegrees ||
         Light->OuterConeAngleDegrees >= 90.0f)) {
        Error = "spot cone angles must satisfy 0 < inner <= outer < 90 degrees";
        return false;
    }
    return true;
}

struct LightComponentMetaRegistration {
    LightComponentMetaRegistration() {
        entt::meta_factory<LightComponent>{}
            .type("light")
            .custom<SceneComponentSchema>(SceneComponentSchema{
                .Create   = &CreateSceneComponent<LightComponent>,
                .Remove   = &RemoveSceneComponent<LightComponent>,
                .Has      = &HasSceneComponent<LightComponent>,
                .Validate = &ValidateLightComponent,
            })
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
