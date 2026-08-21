# Context: WindowSystem

Window-system (WIS) abstraction. GLFW is the current implementation behind
the `IWindowSystem` interface; future backends (Cocoa, WinUI, ...) slot in
without touching consumers.

**Namespace:** `SoulEngine`

## Terms

| Term | Definition |
|------|------------|
| **IWindowSystem** | Abstract window-system interface: window lifecycle, event polling, input frame snapshots, and a window-owned EnTT dispatcher. All methods run on the engine main thread. |
| **WindowSystemType** | RTTI-free implementation tag (`Unknown`, `Glfw`). Consumers dispatch on `GetType()` and `static_cast` to the concrete class. |
| **GlfwWindowSystem** | GLFW implementation. Owns the GLFW library lifetime, one native window, and the window-event dispatcher. `GetNativeHandle()` exposes the raw `GLFWwindow*` for backend adapters and the ImGui GLFW backend. Non-copyable and non-movable. |
| **FramebufferResizeEvent** | Synchronous notification containing the drawable framebuffer extent before and after one platform resize callback. Consumers trigger the initial extent after subscribing. |
| **CreateWindowSystem** | Facade factory returning `UPtr<IWindowSystem>`; owns implementation selection so callers never name a concrete backend. |
| **InputState** | Per-frame input phase: `Unknown`, `Up`, `Pressed`, `Held`, or `Released`. |
| **KeyboardFrameEvent** | One keyboard state snapshot published after platform events are polled. |
| **MouseFrameEvent** | One mouse state snapshot containing button states, cursor position/delta, and scroll accumulation. |
| **WindowKey** | Engine-owned cross-platform keyboard enum covering common letters, numbers, function keys, navigation keys, and modifiers. |
| **WindowMouseButton** | Engine-owned mouse button enum for the supported mouse buttons. |
| **CursorPosition** | Cursor position in window coordinates. |
| **CursorMode** | Cursor presentation/confinement mode: `Normal`, `Hidden`, `Disabled`, or `Captured`. |

## Input Behavior

`GlfwWindowSystem::Create()` captures and hides the GLFW cursor.
`Tick()` runs on the GameLoop's main thread: GLFW callbacks append transitions
to GLFW-specific per-input sequences and accumulate cursor/scroll values, then
the window system publishes one `KeyboardFrameEvent` and one
`MouseFrameEvent`. Each input sequence consumes at most one transition per
tick, so a same-frame press and release exposes `Pressed` first, then
`Released` on the next frame. When a sequence is empty, `Pressed` settles to
`Held` and `Released` settles to `Up`. Window focus loss appends `Release` to
every tracked input sequence. Scroll values are cleared after each mouse frame
event.
Framebuffer callbacks update the extent and synchronously trigger a
`FramebufferResizeEvent`. The ImGui GLFW backend installs its callbacks later
(Editor init) with chaining, so both the window system and ImGui receive every
event.
The editor camera uses `CursorMode::Disabled`; inactive camera input restores
`CursorMode::Normal`.

## Dependencies

- `Core` — logging, config
- Third-party: GLFW
