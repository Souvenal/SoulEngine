# Context: Launch

**Namespace:** `SoulEngine` (exposes `EngineLoop`)

Engine startup and main loop. The entry point binary loads this module and calls its initialization sequence.

## Terms

| Term | Definition |
|------|------------|
| **EngineLoop** | Main engine loop class. Lifecycle: `PreInit` (cmd args + config) -> `Init` (Window -> RHI singleton -> Application) -> `Run` (spawns workers + blocked main-thread loop) -> `Shutdown` (joins workers, tears down). |
| **PreInit** | Processes command-line arguments (`CmdLineArgs`), loads config file (`ConfigManager::LoadFile`), applies log-level config. Currently only uses `CmdLineArgs[0]` (binary path) for config file resolution; argument parsing is extensible for future CLI flags. |
| **Init** | Bootstraps subsystems in order: `CreateWindowSystem` -> `RHIRenderDevice::Create()` -> `TaskGraph::Get().Init()` -> `ResourceManager::Init()` -> `SelectRenderer()` -> `Editor::Create()` / presentation bind -> `OpenApplication`. Any failure tears down prior work and returns `std::unexpected`. Does NOT spawn threads. |
| **Run** | Spawns Render and RHI `std::jthread`s, then calls `GameLoop()` on the calling (main) thread. Blocks until exit. After `GameLoop` returns, calls `Shutdown()`. |
| **GameLoop** | Main-thread loop: `PollEvents` -> compute delta -> wait for slot -> build ImGui frame (which may directly switch Application or Renderer) -> capture the current Renderer -> build `SceneSnapshot` -> `GameReady`. Breaks when `PollEvents()` reports a close request or `m_FatalError` is set by another loop. |
| **RenderLoop** | Worker `std::jthread`. Waits for `GameReady` on its slot, drains render task queue, calls the slot's `IRenderer::Render(SceneSnapshot)`, stores the returned render packet, and sets `RenderReady`. On `Render` failure: broadcasts `FatalError` and exits. |
| **RHILoop** | Worker `std::jthread`. Waits for `RenderReady` on its slot, drains RHI task queue, executes the slot render packet, releases command observers, then sets `RHIDone`. On exit: calls `RHIRenderDevice::WaitIdle()` before returning. |
| **FrameSlot** | Triple-buffered slot (3 fixed). Contains `mutex`, `condition_variable`, `SlotState`, a `SceneSnapshot` copy and shared Renderer for Game→Render handoff, and a `RenderResult` packet for Render→RHI handoff. |
| **Render packet** | `RenderResult`, containing the `RHICommandList` held in `FrameSlot` until RHILoop has completed `RHIRenderDevice::Execute()`. |
| **SlotState** | State machine: `Empty -> GameReady -> RenderReady -> RHIDone -> Empty`. |
| **FatalError** | `std::atomic<bool>` set by any loop on unrecoverable error. Wakes all waiting threads via `notify_all`. Causes `GameLoop` to break and `Run` to fall through to `Shutdown`. |

## Dependencies

- `Core` — logging, config (`ConfigManager`, `LogManager`)
- `WindowSystem` — window system creation (`CreateWindowSystem`, borrowed `IWindowSystem`)
- `Application` — factory + direct current-application lifecycle (`OpenApplication`, `GetCurrentApplication`, `CloseApplication`)
- `RHI` — RHI singleton lifecycle (`RHIRenderDevice::Create`, `Get`, `Destroy`)
- `Scene` — `Scene` mutable world state + `SceneSnapshot` frame slot data
- `Renderer` — engine-global selection/cache (`SelectRenderer`, `GetCurrentRenderer`, `CloseRenderers`) and `IRenderer::Render()` called from `RenderLoop`
- `TaskGraph` — cross-thread task dispatch (`TaskGraph`)
- `Editor` — `Editor` UI frame building (main thread) and UI pass emission (render thread)

## Resource Observer Lifetime

`IRenderer::Render()` resolves command-list resources through
`ResourceManager::TryGetReady()`. The command list stores only RHI observer
pointers, so owning `ResourceRef<T>` values in the live scene/application or
renderer are responsible for keeping logical resource demand alive until the
frame packet is consumed by RHILoop. `SceneSnapshot` carries passive handles
only.

Shutdown releases slots in this order: stop and join threads, reset the
application, clear frame snapshots/render packets, call `ResourceManager::Clear()`,
then destroy `RHIRenderDevice`.

## Known gaps

| Gap | Status |
|-----|--------|
| **CLI argument parsing** — `PreInit` takes `CmdLineArgs` but only uses `args[0]`. No flags, no `--help`, no project path override. | Open |
| **Application abstraction** — `Application` base class + factory pattern is nascent. Only `TestApplication` exists. | Open |
