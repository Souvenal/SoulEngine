module;

#include <entt/entt.hpp>
#include <hlsl++.h>

export module Scene:Components.Core;

export import Core;
export import RHI;

export namespace SoulEngine {

using SceneEntity = entt::entity;

struct Transform {
    hlslpp::float3   Translation    = hlslpp::float3(0.0f, 0.0f, 0.0f);
    hlslpp::float3   Rotation       = hlslpp::float3(0.0f, 0.0f, 0.0f);
    hlslpp::float3   Scale          = hlslpp::float3(1.0f, 1.0f, 1.0f);
    hlslpp::float4x4 WorldTransform = hlslpp::float4x4::identity();

    [[nodiscard]] auto GetLocalMatrix() const -> hlslpp::float4x4 {
        const auto RotationRadians = Rotation * (std::numbers::pi_v<float> / 180.0f);
        return hlslpp::mul(hlslpp::mul(hlslpp::mul(hlslpp::mul(hlslpp::float4x4::scale(Scale),
                                                               hlslpp::float4x4::rotation_x(RotationRadians.x)),
                                                   hlslpp::float4x4::rotation_y(RotationRadians.y)),
                                       hlslpp::float4x4::rotation_z(RotationRadians.z)),
                           hlslpp::float4x4::translation(Translation));
    }
};

/// @brief Immutable render data and resources for one camera/view.
struct RenderViewSnapshot {
    hlslpp::float4x4        ViewProjection = hlslpp::float4x4::identity();
    hlslpp::float3          CameraPosition = hlslpp::float3(0.0f, 0.0f, 0.0f);
    Float32                 ExposureEV100  = 15.0f;
    RHIRef<RHIRenderTarget> ColorRT        = nullptr;
    RHIRef<RHIRenderTarget> DepthRT        = nullptr;
};

struct SceneComponentValidationContext {
    const void* UserData                                                       = nullptr;
    [[nodiscard]] auto (*HasMaterialInstance)(const void*, StringView) -> bool = nullptr;
};

struct SceneComponentSchema {
    entt::meta_any (*Create)(entt::registry&, SceneEntity);
    void (*Remove)(entt::registry&, SceneEntity);
    [[nodiscard]] auto (*Has)(const entt::registry&, SceneEntity) -> bool;
    [[nodiscard]] auto (*Validate)(const SceneComponentValidationContext&, entt::registry&, SceneEntity, String&)
        -> bool;
};

template <typename T>
[[nodiscard]] auto CreateSceneComponent(entt::registry& Registry, SceneEntity Entity) -> entt::meta_any {
    return entt::forward_as_meta(Registry.emplace<T>(Entity));
}

template <typename T>
auto RemoveSceneComponent(entt::registry& Registry, SceneEntity Entity) -> void {
    Registry.remove<T>(Entity);
}

template <typename T>
[[nodiscard]] auto HasSceneComponent(const entt::registry& Registry, SceneEntity Entity) -> bool {
    return Registry.all_of<T>(Entity);
}

} // namespace SoulEngine
