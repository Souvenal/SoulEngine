# ADR 05 - Runtime Resource Lifecycle

## Status

Superseded on July 20, 2026.

## Historical Note

This ADR previously described a `FrameResourceScope` / `ResourcePin<T>` design.
Those APIs no longer exist and must not be used as a reference for current
Resource ownership or command recording.

The current model is:

- `ResourceContext` owns resource entries, slots, and ready payloads.
- `ResourceRef<T>` expresses move-only logical demand.
- `ResourceHandle<T>` is a passive key-and-generation ticket.
- Renderers resolve ready observer pointers through `Resource::Manager` and
  skip draws whose dependencies are not ready.
- `Resource::Mesh` is a cached high-level asset. It publishes after CPU import
  and child-buffer request submission; Renderer waits for each draw packet's
  child buffers before recording an indexed draw.

The authoritative current terminology, lifecycle rules, and extension guide
are in [`Engine/Source/Runtime/Resource/CONTEXT.md`](../../Engine/Source/Runtime/Resource/CONTEXT.md).
