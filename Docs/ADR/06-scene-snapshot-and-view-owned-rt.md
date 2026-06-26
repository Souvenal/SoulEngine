# ADR 06 — SceneSnapshot and View-Owned Render Targets

## Status

Accepted (2026-06-26)

## Context

SoulEngine currently uses a three-thread frame pipeline. Game thread code mutates
application state, Render thread consumes frame data, and RHI thread performs
backend-native GPU work. The current `Scene` concept is overloaded if it tries to
serve both mutable game/editor state and immutable per-frame render data.

At the same time, view-scoped render targets such as a camera depth target are
logically part of the camera/view output contract, not the renderer
implementation. Keeping those targets inside the renderer makes resize handling
and ownership unclear.

The engine also needs a stable rule for resize-driven frame updates: the window
system may emit repeated framebuffer resize notifications, and the runtime must
not assume a single "resize finished" event.

## Decision

SoulEngine splits scene state into two layers:

- `Scene` is the mutable logical container owned by `Application`.
- `SceneSnapshot` is the immutable per-frame render view held by the frame slot
  and consumed by `Renderer`.

`Scene` owns logical world operations such as adding cameras, meshes, materials,
and other runtime objects. `SceneSnapshot` is built from `Scene` after the Game
thread finishes its per-frame update, and it contains only render-consumable
data.

`SceneSnapshot` is produced at the end of the GameLoop, after `OnTick()` has
applied all game-state mutations and resource requests, but before the frame is
published to the Render thread. The frame slot holds this snapshot by value.

`Camera` remains a logical camera type, but it may own view-scoped resource
handles such as `Resource::RenderTargetHandle DepthRT`. The handle represents
camera-owned render target identity and lifecycle, not a backend RHI object.
Renderers consume the handle from `SceneSnapshot` and skip dependent work when
the target is not ready.

For now, the base `Camera` stays intentionally minimal. Specialized camera
forms, such as gameplay cameras, editor viewport cameras, and shadow cameras,
may later be introduced through inheritance or another specialization scheme.
That specialization is deferred and should be tracked explicitly in code with a
detailed TODO comment rather than implemented opportunistically.

Resize handling follows this rule:

- The window system reports framebuffer extent changes.
- The Game thread observes the current extent and requests the needed camera
  render target(s).
- If a newly requested render target is not ready in time for the current
  frame, the renderer skips the dependent pass.
- Old render target handles are released naturally through snapshot lifetime and
  reference counting; they are not force-destroyed from the render path.

## Consequences

This separates mutable gameplay/editor state from render-time snapshot data and
keeps render-thread consumption deterministic. It also makes the camera the
semantic owner of its view-scoped render targets while keeping backend objects
hidden behind resource handles.

The trade-off is an extra snapshot conversion step and a more explicit data
model. That cost is intentional: it prevents the renderer from mutating scene
state, makes resize behavior easier to reason about, and creates a clean place
to add future camera specialization without reworking the frame pipeline.

## Non-Goals

This ADR does not define the final camera inheritance tree.
It does not introduce editor-specific viewport systems.
It does not change how swapchain images are managed inside RHI.

