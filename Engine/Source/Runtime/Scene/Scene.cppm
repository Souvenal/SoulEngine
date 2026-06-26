module;

#include <hlsl++.h>

export module Scene;

export import Core;
export import Resource;
export import std;

using namespace SoulEngine::Core;

export namespace SoulEngine::Scene {

/// @brief Simple camera holding world-space position and orientation.
///
/// Minimal prototype — projection parameters are included so renderers can
/// derive view and projection matrices without depending on math headers.
// TODO: Split Camera into specialized camera types when their ownership and
// projection policies become concrete. Expected variants include gameplay
// cameras, editor viewport cameras, and shadow cameras. Keep this base type
// minimal for now: view parameters plus view-scoped resource refs only.
struct Camera {
    hlslpp::float3 Position    = hlslpp::float3(1.25f, 1.25f, 2.0f);
    hlslpp::float3 Forward     = hlslpp::normalize(hlslpp::float3(0.0f, 0.0f, 0.0f) - Position);
    hlslpp::float3 Up          = hlslpp::float3(0.0f, 1.0f, 0.0f);
    float          FOV         = 60.0f;
    float          NearPlane   = 0.1f;
    float          FarPlane    = 100.0f;
    float          AspectRatio = 16.0f / 9.0f;
    Resource::ResourceRef<RHI::RenderTarget> ColorRT = {};
    Resource::ResourceRef<RHI::RenderTarget> DepthRT = {};

    auto AllocateRenderTargets(Uint32 Width, Uint32 Height) -> void {
        if (Width == 0 || Height == 0) {
            ColorRT.Reset();
            DepthRT.Reset();
            return;
        }

        const auto ColorKey = Format("camera_color_{}x{}", Width, Height);
        const auto DepthKey = Format("camera_depth_{}x{}", Width, Height);
        const auto& ColorHandle = ColorRT.GetHandle();
        const auto& DepthHandle = DepthRT.GetHandle();
        if (ColorHandle.IsValid() && ColorHandle.GetKey() == ColorKey && DepthHandle.IsValid() &&
            DepthHandle.GetKey() == DepthKey) {
            AspectRatio = static_cast<float>(Width) / static_cast<float>(Height);
            return;
        }

        ColorRT = Resource::Manager::Get().RequestRenderTargetRef(
            ColorKey,
            RHI::RenderTargetDesc{
                .Width  = Width,
                .Height = Height,
                .Format = RHI::Format::B8G8R8A8_UNORM,
                .Usage  = RHI::TextureUsage::RenderTarget | RHI::TextureUsage::FrameOutput,
            });
        DepthRT = Resource::Manager::Get().RequestRenderTargetRef(
            DepthKey,
            RHI::RenderTargetDesc{
                .Width  = Width,
                .Height = Height,
                .Format = RHI::Format::D32_SFLOAT,
                .Usage  = RHI::TextureUsage::DepthStencil,
            });
        AspectRatio = static_cast<float>(Width) / static_cast<float>(Height);
    }

    [[nodiscard]] auto GetViewMatrix() const -> hlslpp::float4x4 {
        return hlslpp::float4x4::look_at(Position, Position + Forward, Up);
    }

    /// Vulkan projection: right-handed, zclip [0,1], forward depth, finite far plane.
    [[nodiscard]] auto GetProjectionMatrix() const -> hlslpp::float4x4 {
        float FovRad = FOV * (std::numbers::pi_v<float> / 180.0f);
        return hlslpp::float4x4::perspective(
            hlslpp::projection(hlslpp::frustum::field_of_view_y(FovRad, AspectRatio, NearPlane, FarPlane),
                               hlslpp::zclip::zero,
                               hlslpp::zdirection::forward,
                               hlslpp::zplane::finite));
    }
};

struct SceneSnapshot {
    hlslpp::float3 Position    = hlslpp::float3(1.25f, 1.25f, 2.0f);
    hlslpp::float3 Forward     = hlslpp::normalize(hlslpp::float3(0.0f, 0.0f, 0.0f) - Position);
    hlslpp::float3 Up          = hlslpp::float3(0.0f, 1.0f, 0.0f);
    float          FOV         = 60.0f;
    float          NearPlane   = 0.1f;
    float          FarPlane    = 100.0f;
    float          AspectRatio = 16.0f / 9.0f;
    Resource::ResourceHandle<RHI::RenderTarget> ColorRT = {};
    Resource::ResourceHandle<RHI::RenderTarget> DepthRT = {};
    float Time = 0.0f;

    [[nodiscard]] auto GetViewMatrix() const -> hlslpp::float4x4 {
        return hlslpp::float4x4::look_at(Position, Position + Forward, Up);
    }

    /// Vulkan projection: right-handed, zclip [0,1], forward depth, finite far plane.
    [[nodiscard]] auto GetProjectionMatrix() const -> hlslpp::float4x4 {
        float FovRad = FOV * (std::numbers::pi_v<float> / 180.0f);
        return hlslpp::float4x4::perspective(
            hlslpp::projection(hlslpp::frustum::field_of_view_y(FovRad, AspectRatio, NearPlane, FarPlane),
                               hlslpp::zclip::zero,
                               hlslpp::zdirection::forward,
                               hlslpp::zplane::finite));
    }
};

/// @brief World state container read by renderers each frame.
///
/// Holds the camera and (future) mesh, material, light, and transform
/// collections.
class Scene {
  private:
    std::chrono::steady_clock::time_point m_StartTime = std::chrono::steady_clock::now();

  public:
    Scene()  = default;
    ~Scene() = default;

    Scene(const Scene&)                    = delete;
    auto operator=(const Scene&) -> Scene& = delete;
    Scene(Scene&&)                         = default;
    auto operator=(Scene&&) -> Scene&      = default;

    [[nodiscard]] auto GetElapsedTime() const -> float {
        return std::chrono::duration<float>(std::chrono::steady_clock::now() - m_StartTime).count();
    }

    auto UpdateTime() -> void {
        m_Time = GetElapsedTime();
    }

    /// @brief Register a texture asset path. Application calls this during setup.
    auto AddTexturePath(String Path) -> void {
        m_TexturePaths.emplace_back(std::move(Path));
    }

    /// @brief All registered texture paths.
    [[nodiscard]] auto GetTexturePaths() const -> const std::vector<String>& {
        return m_TexturePaths;
    }

    [[nodiscard]] auto BuildSnapshot() const -> SceneSnapshot {
        return SceneSnapshot{
            .Position    = m_Camera.Position,
            .Forward     = m_Camera.Forward,
            .Up          = m_Camera.Up,
            .FOV         = m_Camera.FOV,
            .NearPlane   = m_Camera.NearPlane,
            .FarPlane    = m_Camera.FarPlane,
            .AspectRatio = m_Camera.AspectRatio,
            .ColorRT     = m_Camera.ColorRT.GetHandle(),
            .DepthRT     = m_Camera.DepthRT.GetHandle(),
            .Time        = m_Time,
        };
    }

    Camera m_Camera;
    float  m_Time = GetElapsedTime();

  private:
    std::vector<String> m_TexturePaths = {};
};

} // namespace SoulEngine::Scene
