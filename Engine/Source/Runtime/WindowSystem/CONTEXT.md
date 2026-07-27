# Context: WindowSystem

Window-system (WIS) abstraction. GLFW is the current implementation behind
the `IWindowSystem` interface; future backends (Cocoa, WinUI, ...) slot in
without touching consumers.

**Namespace:** `SoulEngine`

## Terms

| Term | Definition |
|------|------------|
| **IWindowSystem** | Abstract window-system interface: window lifecycle, event polling, supported-key state queries, scroll accumulation, and relative cursor accumulation. All methods run on the engine main thread. |
| **WindowSystemType** | RTTI-free implementation tag (`Unknown`, `Glfw`). Consumers dispatch on `GetType()` and `static_cast` to the concrete class. |
| **GlfwWindowSystem** | GLFW implementation. Owns the GLFW library lifetime and one native window. `GetNativeHandle()` exposes the raw `GLFWwindow*` for backend adapters and the ImGui GLFW backend. Non-copyable and movable. |
| **CreateWindowSystem** | Facade factory returning `UPtr<IWindowSystem>`; owns implementation selection so callers never name a concrete backend. |
| **WindowKey** | Small engine-owned key enum for the Test application's `W/A/S/D/Q/E` controls. |
| **CursorDelta** | Relative cursor movement accumulated by GLFW callbacks and consumed once per game tick. |

## Input Behavior

`GlfwWindowSystem::Create()` captures and hides the GLFW cursor. `PollEvents()`
runs on the GameLoop's main thread; keyboard state and consume-style scroll /
cursor deltas are read by the application in the same tick. Window callbacks
update only window-system-owned input state. The ImGui GLFW backend installs
its callbacks later (Editor init) with chaining, so both the window system and
ImGui receive every event.

## Dependencies

- `Core` — logging, config
- Third-party: GLFW
