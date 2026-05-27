# Context: Window

Windowing abstraction via GLFW.

**Namespace:** `SoulEngine` (namespace is module-internal; only `WindowDisplay` is exported)

## Terms

| Term | Definition |
|------|------------|
| **WindowDisplay** | Main window abstraction wrapping a GLFW window handle. Owns surface creation for the RHI. Non-copyable, non-movable. |

## Dependencies

- `Core` — logging, config
- Third-party: GLFW
