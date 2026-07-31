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
- Renderers copy ready RHIRef<T> values from Resource wrappers into commands and
  skip draws whose dependencies are not ready.
- `Resource::Mesh` is a cached high-level asset. It publishes after CPU import
  and child-buffer request submission; Renderer waits for each required submesh
  child buffer before recording an indexed draw.

The authoritative current terminology, lifecycle rules, and extension guide
are in [`Engine/Source/Runtime/Resource/CONTEXT.md`](../../Engine/Source/Runtime/Resource/CONTEXT.md).

## Current RHI payload and destruction boundary

**Updated August 1, 2026.** `ResourceRef<T>` remains logical ownership: it
controls demand, deduplication, cache policy, and transient-entry removal. It
is not the lifetime owner of a GPU submission after recording.

A ready Resource wrapper exposes `RHIRef<T>`. The command list copies that ref;
Vulkan retains the submitted list until its graphics timeline completion point.
Consequently ResourceManager may release a transient logical entry after
submission without destroying a native payload that the GPU still uses.

The final `RHIRef` release enqueues native destruction into the render-device
queue. Normal-runtime RHILoop calls `RHIRenderDevice::Tick()` to drain that
queue. All Resource-owned refs must release before `RHIRenderDevice::Destroy()`;
the current global deletion queue has no safe post-device fallback.
