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
| **GameLoop** | Main-thread loop: `PollEvents` -> compute delta -> wait for slot -> `OnTick(dt)` -> build `SceneSnapshot` -> `GameReady`. Breaks when `PollEvents()` reports a close request or `m_FatalError` is set by another loop. |
| **RenderLoop** | Worker `std::jthread`. Waits for `GameReady` on its slot, drains render task queue, calls `Renderer::Render(SceneSnapshot)`, stores the returned render packet, and sets `RenderReady`. On `Render` failure: broadcasts `FatalError` and exits. |
| **RHILoop** | Worker `std::jthread`. Waits for `RenderReady` on its slot, drains RHI task queue, executes the slot render packet, releases command observers and frame pins, then sets `RHIDone`. On exit: calls `RenderDevice::WaitIdle()` before returning. |
| **FrameSlot** | Triple-buffered slot (3 fixed). Contains `mutex`, `condition_variable`, `SlotState`, a `SceneSnapshot` copy for Game→Render handoff, and a `Renderer::RenderResult` packet for Render→RHI handoff. |
| **Render packet** | `Renderer::RenderResult`, containing `RHI::CommandList` plus `Resource::FrameResourceScope`. It is held in `FrameSlot` until RHILoop has completed `RenderDevice::Execute()`. |
| **SlotState** | State machine: `Empty -> GameReady -> RenderReady -> RHIDone -> Empty`. |
| **FatalError** | `std::atomic<bool>` set by any loop on unrecoverable error. Wakes all waiting threads via `notify_all`. Causes `GameLoop` to break and `Run` to fall through to `Shutdown`. |
| **SwitchApplication** | Replaces the current application. Calls `OnDetach()` on the old app, then `Application::Create(Name)` + `OnAttach()` for the new one. |

## Dependencies

- `Core` — logging, config (`ConfigManager`, `LogManager`)
- `Window` — window display creation (`WindowDisplay`)
- `Application` — factory + lifecycle (`Application::Create`, `OnAttach`, `OnDetach`, `OnTick`)
- `RHI` — RHI singleton lifecycle (`RenderDevice::Create`, `Get`, `Destroy`)
- `Scene` — `Scene::Scene` mutable world state + `SceneSnapshot` frame slot data
- `Renderer` — `IRenderer::Render()` called from `RenderLoop`
- `TaskGraph` — cross-thread task dispatch (`SoulEngine::TaskGraph`)

## Resource Pin Lifetime

`Renderer::Render()` resolves command-list resources through
`Resource::FrameResourceScope::Acquire()`.
The command list stores only RHI observer pointers. `FrameSlot::RenderPacket`
therefore keeps the corresponding `FrameResourceScope` alive from RenderLoop
publication through RHILoop execution. RHILoop clears the packet only after
`RenderDevice::Execute()` and `FrameMark`, then asks `ResourceManager` to
collect released transient resources whose refs and pins have both dropped.

Shutdown releases slots in this order: stop and join threads, detach/reset the
application, clear frame snapshots/render packets/pins, call
`ResourceManager::Clear()`, then destroy `RenderDevice`.

## Known gaps

| Gap | Status |
|-----|--------|
| **CLI argument parsing** — `PreInit` takes `CmdLineArgs` but only uses `args[0]`. No flags, no `--help`, no project path override. | Open |
| **Application abstraction** — `Application` base class + factory pattern is nascent. Only `TestApplication` exists. | Open |
