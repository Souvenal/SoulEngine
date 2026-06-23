# Context: Renderer

**Namespace:** `SoulEngine::Renderer`

Render pipeline orchestration layer — drives RHI command execution and pass composition.

## Terms

| Term | Definition |
|------|------------|
| **IRenderer** | Abstract base class for all renderers. Holds `m_Passes`. Defines `OnAttach()`, `OnDetach()`, `Render(const Scene::Scene&) -> expected<CommandList, ErrorMessage>`. Returns a command list — does NOT touch BeginFrame/EndFrame. |
| **TestRenderer** | Prototype concrete renderer. Populates `m_Passes` with one `GraphicsPass`. Emits variant commands via Pass builder methods. |
| **GraphicsPass** | Stateless pipeline wrapper: shader pair → ShaderCache → RHI pipeline. Created via `GraphicsPass::Create(GraphicsPassDesc)`. |
| **GraphicsPassDesc** | Description: `VertEntry` + `FragEntry`, each `ShaderEntry{ShaderPath, EntryName}`. |
| **Pass dependency** | Resource dependency whose absence makes an entire pass incoherent for the current frame. |
| **Draw dependency** | Resource dependency whose absence affects one draw packet or material use, not necessarily the entire pass. |

## Architecture

```
IRenderer
 ├─ m_Passes: std::vector<GraphicsPass>
 │
 ├── TestRenderer     (prototype — single quad pass, returns CommandList)
 ├── ForwardRenderer  (future)
 ├── DeferredRenderer (future)
 └── RayTracingRenderer (future)
```

## Dependencies

- `Core` — logging, config
- `RHI` — `RenderDevice::Get()`, `CommandList`, pipeline types
- `Scene` — `Scene::Scene` (read-only per frame)
- `ShaderCache` — shader compilation caching

## Relationships

- **Application** owns `UPtr<IRenderer>`, creates it in `OnAttach()`, calls `Render(m_Scene)` from `OnRender()`. Application no longer calls BeginFrame/EndFrame — those moved to `RenderDevice::Execute()` on RHIThread.
- **EngineLoop** owns the frame pipeline: GameLoop calls `OnTick()`, RenderLoop calls `Renderer::Render()` -> gets back `CommandList`, RHILoop calls `RenderDevice::Execute(CommandList)`.
- Pipeline lifecycle (destroy on detach) is the renderer's responsibility, not GraphicsPass's.
- Resource readiness is resolved before emitting coherent RHI commands. Pass dependencies decide whether a pass is emitted; draw dependencies decide whether an individual draw is emitted, skipped, or substituted with fallback resources.
