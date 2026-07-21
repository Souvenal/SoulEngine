# Context: Application

**Namespace:** `SoulEngine::Application`

Application logic lifecycle. Owns the mutable scene and renderer.

## Terms

| Term | Definition |
|------|------------|
| **Application** | Project-level engine unit created through `ApplicationFactory`. Owns the mutable Scene and configured default Renderer, including their attach/detach lifecycle. |
| **Default Scene Document** | Mandatory `Scene.yaml` at `Applications/<ApplicationName>/`. Every Application automatically loads it as its initial Scene during attachment; a missing document or Structural Error prevents startup, while Component Warnings are logged and allow startup. |
| **Application Key** | Factory key that identifies one project-level Application and selects its `Applications/<ApplicationName>/` directory. |
| **SceneSnapshot** | Immutable render-facing copy of `Scene` built at the end of `OnTick()` and published to the frame slot. |
| **OnAttach** | Non-virtual lifecycle method. Loads and validates the Default Scene Document before it initializes the configured default Renderer. Called by EngineLoop after the RHI singleton is ready. |
| **OnDetach** | Non-virtual lifecycle method. Detaches and releases the configured default Renderer before engine shutdown. |
| **Attach Rollback** | If default Renderer creation or attachment fails, `OnAttach()` detaches and releases any partially initialized Renderer before it returns the failure. |
| **OnTick** | The temporary sole virtual Application extension point for project-specific per-frame behavior and main-thread Window input consumption. It remains until an explicit systems/controllers mechanism replaces it. |
| **OnRender** | Non-virtual. Fixed pipeline: calls `m_Renderer->Render(SceneSnapshot)`. |
| **Create** | Static factory: looks up `Name` in `ApplicationFactory`, constructs the application. Does NOT call `OnAttach()` — EngineLoop controls attach/detach timing. |

## Dependencies

- `Core` — logging, config, `Factory`, `Singleton`
- `Renderer` — `IRenderer` (owns via UPtr)
- `Scene` — `Scene::Scene` (owns by value)
- `Window` — borrowed `WindowDisplay` passed to `OnTick`

## Relationships

- **Application** does not own the window, RHI context, or GPU resources.
- **Application** may consume the borrowed `WindowDisplay` during `OnTick`, but
  must not retain it beyond that call.
- **EngineLoop** creates the RHI singleton, creates applications via `Application::Create()`, and calls `OnAttach()`/`OnDetach()` at the right points.
- **Application** owns the mutable scene that is converted into a per-frame `SceneSnapshot`.
- **GameLoop** is responsible for building the `SceneSnapshot` at the end of `OnTick()` before publishing the frame slot.

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
