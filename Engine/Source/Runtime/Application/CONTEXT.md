# Context: Application

**Namespace:** `SoulEngine::Application`

Application logic lifecycle. Owns the scene and renderer.

## Terms

| Term | Definition |
|------|------------|
| **Application** | Top-level object created by `EngineLoop`. Owns `Scene::Scene m_Scene` and `UPtr<Renderer::IRenderer> m_Renderer`. Lifecycle: `Create(Name)` → `OnAttach()` → `OnTick(dt)` / `OnRender()` → `OnDetach()`. |
| **OnAttach** | Pure virtual. Derived class constructs the scene and renderer. Called by EngineLoop after RHI singleton is ready. |
| **OnDetach** | Pure virtual. Derived class destroys the renderer and releases owned resources. Called by EngineLoop before RHI singleton shutdown. |
| **OnTick** | Pure virtual. Per-frame application update for simulation and state changes. |
| **OnRender** | Non-virtual. Fixed pipeline: calls `m_Renderer->Render(m_Scene)`. |
| **Create** | Static factory: looks up `Name` in `ApplicationFactory`, constructs the application. Does NOT call `OnAttach()` — EngineLoop controls attach/detach timing. |

## Dependencies

- `Core` — logging, config, `Factory`, `Singleton`
- `Renderer` — `IRenderer` (owns via UPtr)
- `Scene` — `Scene::Scene` (owns by value)

## Relationships

- **Application** does not own the window, RHI context, or GPU resources.
- **EngineLoop** creates the RHI singleton, creates applications via `Application::Create()`, and calls `OnAttach()`/`OnDetach()` at the right points.
- **Application** owns the scene that the renderer reads each frame.

## Architecture

Applications self-register with `ApplicationFactory` via `AutoRegistrar` in
standalone modules under `Applications/`. The facade (`Application.cppm`)
never imports or constructs concrete application types.

| Piece | Location | Purpose |
|-------|----------|---------|
| `ApplicationFactory` | `Application.cppm` | `Core::Factory<Application>` — singleton-backed registry |
| `Application::Create` | `Application.cppm` | Static factory — factory lookup + construction. Returns `std::expected<UPtr<Application>, ErrorMessage>`. |
| `TestApplication` | `Applications/TestApp.cppm` | Standalone `export module TestApp;` — self-registers with `{"Test"}` key |

## Adding a New Application

1. Create `Applications/MyApp.cppm` with `export module MyApp;`
2. `import Application; import Core; import Renderer; import Scene;`
3. `class MyApp final : public Application::Application { ... };`
4. `Application::ApplicationFactory::AutoRegistrar<MyApp> s_Reg{"MyApp"};`
5. Done — zero changes to `Application.cppm`
