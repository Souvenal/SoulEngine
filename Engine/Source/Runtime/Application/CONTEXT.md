# Context: Application

**Namespace:** `SoulEngine::Application`

Application project identity and scene ownership.

## Terms

| Term | Definition |
|------|------------|
| **Application** | Project-level engine unit created through `ApplicationFactory`. Owns the mutable Scene and project identity. |
| **Default Scene Document** | Mandatory `Scene.yaml` at `Applications/<ApplicationName>/`. Every Application automatically loads it as its initial Scene during creation; a missing document or Structural Error prevents startup, while Component Warnings are logged and allow startup. |
| **Application Key** | Factory key that identifies one project-level Application and selects its `Applications/<ApplicationName>/` directory. |
| **SceneSnapshot** | Immutable render-facing copy of `Scene` built by GameLoop and published to the frame slot. |
| **Create** | Static factory: looks up `Name` in `ApplicationFactory`, constructs the application, and loads the Default Scene Document. |

## Dependencies

- `Core` — logging, config, `Factory`, `Singleton`
- `Scene` — `Scene::Scene` (owns by value)

## Relationships

- **Application** does not own the window, RHI context, renderer, or GPU resources.
- **Application** owns the process-wide current application through module-private storage. `OpenApplication()` creates a fully loaded candidate before replacing the current application, so a failed open preserves the current application.
- **EngineLoop** creates the RHI singleton and retrieves the current application only on the game thread for tick and snapshot scheduling.
- **Application** owns the mutable scene that is converted into a per-frame `SceneSnapshot`.
- **GameLoop** is responsible for building the `SceneSnapshot` before publishing the frame slot.

## Architecture

Applications self-register with `ApplicationFactory` via `AutoRegistrar` in
standalone modules under `Applications/`. The facade (`Application.cppm`)
never imports or constructs concrete application types.

| Piece | Location | Purpose |
|-------|----------|---------|
| `ApplicationFactory` | `Application.cppm` | `Core::Factory<Application>` — singleton-backed registry |
| `Application::Create` | `Application.cppm` | Static factory — factory lookup + construction. Returns `std::expected<UPtr<Application>, ErrorMessage>`. |
| `TestApplication` | `Applications/TestApp.cppm` | Standalone `export module TestApp;` — self-registers with `{"Test"}` key |
| `VikingApplication` | `Applications/VikingApp.cppm` | Standalone `export module VikingApp;` — self-registers with `{"Viking"}` key |

## Adding a New Application

1. Create `Applications/MyApp.cppm` with `export module MyApp;`
2. `import Application; import Core; import Scene;`
3. `class MyApp final : public Application::Application { ... };`
4. `Application::ApplicationFactory::AutoRegistrar<MyApp> s_Reg{"MyApp"};`
5. Done — zero changes to `Application.cppm`
