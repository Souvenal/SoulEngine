# Context: Renderer

**Namespace:** `SoulEngine::Renderer`

Render pipeline orchestration layer — drives RHI command execution and pass composition.

## Terms

| Term | Definition |
|------|------------|
| **IRenderer** | Abstract base class for all renderers. Defines `OnAttach()`, `OnDetach()`, `Render(const Scene::Scene&) -> expected<CommandList, ErrorMessage>`. Returns a command list — does NOT touch BeginFrame/EndFrame. |
| **TestRenderer** | Prototype concrete renderer. Owns async pipeline/texture/vertex/index buffer handles and emits variant commands when dependencies are ready. |
| **Pass dependency** | Resource dependency whose absence makes an entire pass incoherent for the current frame. |
| **Draw dependency** | Resource dependency whose absence affects one draw packet or material use, not necessarily the entire pass. |
| **Draw material data** | Per-draw or per-material shader data that selects resources or parameters for one draw packet. |

## Architecture

```
IRenderer
 ├── TestRenderer     (prototype — single quad pass, returns CommandList)
 ├── ForwardRenderer  (future)
 ├── DeferredRenderer (future)
 └── RayTracingRenderer (future)
```

## Dependencies

- `Core` — logging, config
- `RHI` — `RenderDevice::Get()`, `CommandList`, pipeline types
- `Scene` — `Scene::Scene` (read-only per frame)
- `Resource` — async texture, buffer, and pipeline requests

## Relationships

- **Application** owns `UPtr<IRenderer>`, creates it in `OnAttach()`, calls `Render(m_Scene)` from `OnRender()`. Application no longer calls BeginFrame/EndFrame — those moved to `RenderDevice::Execute()` on RHIThread.
- **EngineLoop** owns the frame pipeline: GameLoop calls `OnTick()`, RenderLoop calls `Renderer::Render()` -> gets back `CommandList`, RHILoop calls `RenderDevice::Execute(CommandList)`.
- Pipeline, texture, and buffer lifetime flow through `Resource::Manager` handles; sync resource creation is not done in renderer attach or render-frame code.
- Resource readiness is resolved before emitting coherent RHI commands. Pass dependencies decide whether a pass is emitted; draw dependencies decide whether an individual draw is emitted, skipped, or substituted with fallback resources.
- Bindless texture selection is **Draw material data**, not a shader entry-point identity or pipeline compile-time interface.
