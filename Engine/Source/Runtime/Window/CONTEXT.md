# Context: Window

Windowing abstraction via GLFW.

**Namespace:** `SoulEngine` (namespace is module-internal; only `WindowDisplay` is exported)

## Terms

| Term | Definition |
|------|------------|
| **WindowDisplay** | Main GLFW window abstraction wrapping a native handle. Owns surface creation, event polling, supported-key state queries, scroll accumulation, and relative cursor accumulation. Non-copyable and movable. |
| **WindowKey** | Small engine-owned key enum for the Test application's `W/A/S/D/Q/E` controls. |
| **CursorDelta** | Relative cursor movement accumulated by GLFW callbacks and consumed once per game tick. |

## Input Behavior

`WindowDisplay::Create()` captures and hides the GLFW cursor. `PollEvents()`
runs on the GameLoop's main thread; keyboard state and consume-style scroll /
cursor deltas are read by the application in the same tick. Window callbacks
update only WindowDisplay-owned input state.

## Dependencies

- `Core` — logging, config
- Third-party: GLFW
