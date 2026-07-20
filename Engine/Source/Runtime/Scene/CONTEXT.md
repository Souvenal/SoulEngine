# Context: Scene

**Namespace:** `SoulEngine::Scene`

Mutable world-state container for runtime and editor logic. Owned by
Application, read by Renderer through a per-frame `SceneSnapshot`.

## Terms

| Term | Definition |
|------|------------|
| **Scene** | Concrete mutable data container owned by Application. Holds logical cameras, meshes, materials, lights, transforms, and other runtime/editor state. |
| **SceneSnapshot** | Immutable per-frame render view built from `Scene` at the end of the GameLoop and held by the frame slot. It contains one `RenderViewSnapshot` per camera/view. Renderer consumes this snapshot, not the mutable `Scene`. |
| **RenderViewSnapshot** | One immutable camera/view record: view-projection data plus passive handles for color/depth targets and that view's logical constant buffer. |
| **Draw packet** | Passive submesh draw record containing SOA vertex/index-buffer handles and index count. Renderer resolves its handles on the render thread. |
| **Camera** | Logical camera entity. Owns world-space view parameters, yaw/pitch orientation state, movement/rotation helpers, and view-scoped `ResourceRef` values for color/depth render targets and the view constant buffer. Concurrent cameras must use distinct view constant-buffer keys. |

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

`Scene` owns Mesh `ResourceRef` values. `BuildSnapshot()` expands every ready
Mesh into passive draw packets; a packet does not keep its child buffers alive,
so Renderer checks each handle before recording the draw.

The test camera moves on its horizontal forward/right plane with `WASD`, moves
on world up with `Q/E`, zooms along forward from wheel input, and derives
forward from yaw/pitch cursor rotation.

## Dependencies

- `Core` — types, error handling
- `Resource` — typed resource refs and snapshot handles used by camera-owned view outputs
- `hlsl++` — vector and matrix math for Camera
