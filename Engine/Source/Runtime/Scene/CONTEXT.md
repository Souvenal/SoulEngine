# Context: Scene

**Namespace:** `SoulEngine::Scene`

Mutable world-state container for runtime and editor logic. Owned by
Application, read by Renderer through a per-frame `SceneSnapshot`.

## Terms

| Term | Definition |
|------|------------|
| **Scene** | Concrete mutable data container owned by Application. Holds logical cameras, meshes, materials, lights, transforms, and other runtime/editor state. |
| **SceneSnapshot** | Immutable per-frame render view built from `Scene` at the end of the GameLoop and held by the frame slot. Renderer consumes this snapshot, not the mutable `Scene`. |
| **Camera** | Logical camera entity. Owns world-space view parameters and may also own view-scoped `ResourceRef` values such as color and depth render target refs. |

## Architecture

`Scene` is a mutable data container owned by Application. `SceneSnapshot` is
the immutable render-facing view copied into the frame slot at the end of the
GameLoop. The renderer consumes `SceneSnapshot` each frame via
`IRenderer::Render()`.

`Scene` itself does not own backend RHI objects, but `Camera` is allowed to own
view-scoped `ResourceRef` values that keep render targets requested while that
camera needs them. The camera color render target is the normal frame output
consumed by `RHI::CommandList::PresentSource`; swapchain images stay backend
private. `SceneSnapshot` carries passive `ResourceHandle` copies for
render-thread acquisition; those handles do not keep payloads alive.

## Dependencies

- `Core` — types, error handling
- `Resource` — typed resource refs and snapshot handles used by camera-owned view outputs
- `hlsl++` — vector and matrix math for Camera
