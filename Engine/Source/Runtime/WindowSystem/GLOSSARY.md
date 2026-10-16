# WindowSystem

Window-system abstraction: window lifecycle, event polling, and per-frame input snapshots behind one interface. GLFW is the current implementation; future backends slot in without touching consumers.

## Language

**IWindowSystem**:
Abstract window-system interface: window lifecycle, event polling, input frame snapshots, and a window-owned event dispatcher. All methods run on the engine main thread.

**WindowSystemType**:
RTTI-free implementation tag; consumers dispatch on it and cast to the concrete class.

**GlfwWindowSystem**:
GLFW implementation — owns the GLFW library lifetime, one native window, and the window-event dispatcher, and exposes the raw native handle for backend adapters.

**CreateWindowSystem**:
Facade factory returning the window system; owns implementation selection so callers never name a concrete backend.

**InputState**:
Per-frame input phase: Up, Pressed, Held, or Released. A same-frame press and release reports Pressed first and Released on the next frame; Pressed settles to Held and Released to Up.

**KeyboardFrameEvent / MouseFrameEvent**:
One keyboard or mouse state snapshot published after platform events are polled; the mouse snapshot carries button states, cursor position and delta, and scroll accumulation.

**WindowKey / WindowMouseButton**:
Engine-owned cross-platform keyboard and mouse button enums.
_Avoid_: platform-native key codes in engine code

**CursorMode**:
Cursor presentation/confinement mode: Normal, Hidden, Disabled, or Captured.

**FramebufferResizeEvent**:
Synchronous notification carrying the drawable framebuffer extent before and after a platform resize.
