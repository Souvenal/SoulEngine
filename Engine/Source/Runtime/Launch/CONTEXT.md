# Context: Launch

**Namespace:** `SoulEngine::Launch` (exposes `EngineLoop`)

Engine startup and main loop. The entry point binary loads this module and calls its initialization sequence.

## Terms

| Term | Definition |
|------|------------|
| **EngineLoop** | Main engine loop class. Lifecycle: `PreInit` (cmd args + config) -> `Init` (Window -> RHI singleton -> Application) -> `Run` (spawns workers + blocked main-thread loop) -> `Shutdown` (joins workers, tears down). |
| **PreInit** | Processes command-line arguments (`CmdLineArgs`), loads config file (`ConfigManager::LoadFile`), applies log-level config. Currently only uses `CmdLineArgs[0]` (binary path) for config file resolution; argument parsing is extensible for future CLI flags. |
| **Init** | Bootstraps subsystems in order: `WindowDisplay::Create` -> `RHI::RenderDevice::Create(Window)` -> `SwitchApplication`. Any failure tears down prior work and returns `std::unexpected`. Does NOT spawn threads. |
| **Run** | Spawns Render and RHI `std::jthread`s, then calls `GameLoop()` on the calling (main) thread. Blocks until exit. After `GameLoop` returns, calls `Shutdown()`. |
| **GameLoop** | Main-thread loop: `PollEvents` -> compute delta -> wait for slot -> `OnTick(dt)` -> write scene -> `GameReady`. Also checks `WindowDisplay.IsExitRequested()` (close button) and `m_FatalError` (render crash). |
| **RenderLoop** | Worker `std::jthread`. Waits for `GameReady` on its slot, drains render task queue, calls `Renderer::Render(scene)`, sets `RenderReady`. On `Render` failure: broadcasts `FatalError` and exits. |
| **RHILoop** | Worker `std::jthread`. Waits for `RenderReady` on its slot, drains RHI task queue, sets `RHIDone`. On exit: calls `RenderDevice::WaitIdle()` before returning. |
| **FrameSlot** | Triple-buffered slot (3 fixed). Contains `mutex`, `condition_variable`, `SlotState`, and a `Scene::Scene` copy for Game→Render handoff. |
| **SlotState** | State machine: `Empty -> GameReady -> RenderReady -> RHIDone -> Empty`. |
| **FatalError** | `std::atomic<bool>` set by any loop on unrecoverable error. Wakes all waiting threads via `notify_all`. Causes `GameLoop` to break and `Run` to fall through to `Shutdown`. |
| **SwitchApplication** | Replaces the current application. Calls `OnDetach()` on the old app, then `Application::Create(Name)` + `OnAttach()` for the new one. |

## Dependencies

- `Core` — logging, config (`ConfigManager`, `LogManager`)
- `Window` — window display creation (`WindowDisplay`)
- `Application` — factory + lifecycle (`Application::Create`, `OnAttach`, `OnDetach`, `OnTick`)
- `RHI` — RHI singleton lifecycle (`RenderDevice::Create`, `Get`, `Destroy`)
- `Scene` — `Scene::Scene` type for frame slot data
- `Renderer` — `IRenderer::Render()` called from `RenderLoop`
- `TaskGraph` — cross-thread task dispatch (`SoulEngine::TaskGraph`)

## Known gaps

| Gap | Status |
|-----|--------|
| **CLI argument parsing** — `PreInit` takes `CmdLineArgs` but only uses `args[0]`. No flags, no `--help`, no project path override. | Open |
| **Application abstraction** — `Application` base class + factory pattern is nascent. Only `TestApplication` exists. | Open |
