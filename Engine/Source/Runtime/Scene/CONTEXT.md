# Context: Scene

**Namespace:** `SoulEngine::Scene`

World state container for per-frame rendering.  Owned by Application, read by
Renderer via `Render(const Scene&)`.

## Terms

| Term | Definition |
|------|------------|
| **Scene** | Concrete data container owned by Application. Holds the camera and (future) mesh, material, light, and transform collections. Scene has no awareness of RHI, Renderer, or rendering concepts. |
| **Camera** | World-space view/projection description: `Position`, `Forward`, `Up`, FOV, near/far planes, and aspect ratio. |

## Architecture

The Scene is a data container owned by Application.  The renderer receives
a `const Scene&` each frame via `IRenderer::Render()`.  Scene has no
awareness of RHI, Renderer, or rendering concepts.

## Dependencies

- `Core` — types, error handling
- `hlsl++` — vector and matrix math for Camera
