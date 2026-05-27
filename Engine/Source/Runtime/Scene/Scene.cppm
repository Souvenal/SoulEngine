module;

#include <glm/glm.hpp>

export module Scene;

export import std;

export namespace SoulEngine::Scene {

/// @brief Simple camera holding world-space position and orientation.
///
/// Minimal prototype — FOV, near/far planes, and projection will be added
/// as the renderer evolves beyond the triangle pass.
struct Camera {
    glm::vec3 Position = glm::vec3(0.0f);
    glm::vec3 Forward  = glm::vec3(0.0f, 0.0f, -1.0f);
    glm::vec3 Up       = glm::vec3(0.0f, 1.0f, 0.0f);
};

/// @brief World state container read by renderers each frame.
///
/// Holds the camera and (future) mesh, material, light, and transform
/// collections.  Scene has no awareness of RHI, Renderer, or rendering
/// concepts — it is a pure data container.
class Scene {
  public:
    Scene()  = default;
    ~Scene() = default;

    Scene(const Scene&)                    = delete;
    auto operator=(const Scene&) -> Scene& = delete;
    Scene(Scene&&)                         = delete;
    auto operator=(Scene&&) -> Scene&      = delete;

    Camera m_Camera;
};

} // namespace SoulEngine::Scene
