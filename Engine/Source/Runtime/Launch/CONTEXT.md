# Context: Launch

**Namespace:** `SoulEngine::Launch` (exposes `EngineLoop`)

Engine startup and main loop. The entry point binary loads this module and calls its initialization sequence.

## Terms

| Term | Definition |
|------|------------|
| **EngineLoop** | Main engine loop class. Lifecycle: `PreInit` (cmd args + config) -> `Init` (Window -> RHI singleton -> Application) -> `Tick` loop -> `Shutdown`. |
| **PreInit** | Processes command-line arguments (`CmdLineArgs`), loads config file (`ConfigManager::LoadFile`), applies log-level config. Currently only uses `CmdLineArgs[0]` (binary path) for config file resolution; argument parsing is extensible for future CLI flags. |
| **Init** | Bootstraps subsystems in order: `WindowDisplay::Create` -> `RHI::RenderDevice::Create(Window)` -> `SwitchApplication`. Any failure tears down prior work and returns `std::unexpected`. |
| **SwitchApplication** | Replaces the current application while preserving the existing window and RHI singleton. Calls `OnDetach()` on the old app, then `Application::Create(Name)` + `OnAttach()` for the new one. |
| **Tick** | Per-frame: `WindowDisplay::PollEvents()` → `Application::OnTick(dt)` → `Application::OnRender()`. |

## Dependencies

- `Core` — logging, config (`ConfigManager`, `LogManager`)
- `Window` — window display creation (`WindowDisplay`)
- `Application` — factory + lifecycle (`Application::Create`, `OnAttach`, `OnDetach`, `OnTick`, `OnRender`)
- `RHI` — RHI singleton lifecycle (`RenderDevice::Create`, `Get`, `Shutdown`)

## Known gaps

| Gap | Status |
|-----|--------|
| **CLI argument parsing** — `PreInit` takes `CmdLineArgs` but only uses `args[0]`. No flags, no `--help`, no project path override. | Open |
| **Application abstraction** — `Application` base class + factory pattern is nascent. Only `TestApplication` exists. | Open |
