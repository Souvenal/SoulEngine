module;

#include <entt/entt.hpp>
#include <hlsl++.h>

module Scene.Light;

namespace SoulEngine {

[[nodiscard]] auto LightComponentFields() -> FieldTable {
    static constexpr FieldDescriptor Table[] = {
        Field<&LightComponent::SetType, &LightComponent::GetType>("type"),
        Field<&LightComponent::ColorR>("color_r"),
        Field<&LightComponent::ColorG>("color_g"),
        Field<&LightComponent::ColorB>("color_b"),
        Field<&LightComponent::Intensity>("intensity"),
        Field<&LightComponent::RangeMeters>("range_meters"),
    };
    return Table;
}

auto LightSystem::OnUpdate(Float32 DeltaTime) -> void {
    // LightSystem doesn't need per-frame updates currently
}

auto LightSystem::CollectLights() const -> std::vector<LightRecord> {
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
            .RangeMeters = Light.RangeMeters,
            .Direction   = Direction,
        });
    }

    return Lights;
}

} // namespace SoulEngine
