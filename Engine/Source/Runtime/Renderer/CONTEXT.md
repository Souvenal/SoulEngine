# Context: Renderer

**Namespace:** `SoulEngine::Renderer`

Render pipeline orchestration layer — drives RHI command execution and pass composition.

## Terms

| Term | Definition |
|------|------------|
| **IRenderer** | Abstract base class for all renderers. Holds `m_Passes`. Defines `OnAttach()`, `OnDetach()`, `Render(const Scene::Scene&)`. Receives scene data per-frame — does NOT own the scene. |
| **TestRenderer** | Prototype concrete renderer. Populates `m_Passes` with one `GraphicsPass`. Accesses RHI context via `RHI::RenderDevice::Get()` singleton. |
| **GraphicsPass** | Stateless pipeline wrapper: shader pair → ShaderCache → RHI pipeline. Created via `GraphicsPass::Create(GraphicsPassDesc)`. Does NOT own the pipeline resource. |
| **GraphicsPassDesc** | Description: `VertEntry` + `FragEntry`, each `ShaderEntry{ShaderPath, EntryName}`. |

## Architecture

```
IRenderer
 ├─ m_Passes: std::vector<GraphicsPass>
 │
 ├── TestRenderer     (prototype — single quad pass)
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

- **Application** owns `UPtr<IRenderer>`, creates it in `OnAttach()`, calls `Render(m_Scene)` from `OnRender()`.
- **EngineLoop** creates the RHI singleton via `RHI::RenderDevice::Create()` before any application is attached.
- Pipeline lifecycle (destroy on detach) is the renderer's responsibility, not GraphicsPass's.
