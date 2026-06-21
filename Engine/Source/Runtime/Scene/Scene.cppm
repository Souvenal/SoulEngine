module;

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

export module Scene;

export import std;

export namespace SoulEngine::Scene {

/// @brief Simple camera holding world-space position and orientation.
///
/// Minimal prototype — projection parameters are included so renderers can
/// derive view and projection matrices without depending on math headers.
struct Camera {
    glm::vec3 Position    = glm::vec3(1.25f, 1.25f, 2.0f);
    glm::vec3 Forward     = glm::normalize(glm::vec3(0.0f) - Position);
    glm::vec3 Up          = glm::vec3(0.0f, 1.0f, 0.0f);
    float     FOV         = 60.0f;
    float     NearPlane   = 0.1f;
    float     FarPlane    = 100.0f;
    float     AspectRatio = 16.0f / 9.0f;

    [[nodiscard]] auto GetViewMatrix() const -> glm::mat4 {
        return glm::lookAt(Position, Position + Forward, Up);
    }

    [[nodiscard]] auto GetProjectionMatrix() const -> glm::mat4 {
        return glm::perspective(
            glm::radians(FOV), AspectRatio, NearPlane, FarPlane);
    }
};

/// @brief World state container read by renderers each frame.
///
/// Holds the camera and (future) mesh, material, light, and transform
/// collections.  Scene has no awareness of RHI, Renderer, or rendering
/// concepts — it is a pure data container.
class Scene {
  private:
    std::chrono::steady_clock::time_point m_StartTime = std::chrono::steady_clock::now();

  public:
    Scene()  = default;
    ~Scene() = default;

    Scene(const Scene&)                    = default;
    auto operator=(const Scene&) -> Scene& = default;
    Scene(Scene&&)                         = default;
    auto operator=(Scene&&) -> Scene&      = default;

    [[nodiscard]] auto GetElapsedTime() const -> float {
        return std::chrono::duration<float>(std::chrono::steady_clock::now() - m_StartTime).count();
    }

    auto UpdateTime() -> void {
        m_Time = GetElapsedTime();
    }

    Camera m_Camera;
    float  m_Time = GetElapsedTime();
};

} // namespace SoulEngine::Scene
