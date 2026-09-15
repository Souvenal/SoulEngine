# ADR 05 — RenderGraph Pooled Transients, Derived Usage, and Explicit Present

## Status

Accepted (2026-09-15). Partially supersedes [ADR 06](06-scene-snapshot-and-view-owned-rt.md)
(view-owned render-target ownership; the Scene/SceneSnapshot layering stands).

## Context

View-scoped render targets (GBuffer, SceneColor) were created by the Scene
module's `CameraRenderTargetsLoader` with a hard-coded union of RHI usage flags
covering every renderer. Whether a target needs `RenderTarget` or
`ShaderStorage` is known only by the passes that consume it: Raster binds the
GBuffer as attachments, RayTracing writes Normal/EntityId as storage images.
Creation-time usage was therefore a guess made by the wrong layer, paid for by
every renderer.

The graph realized transients with a fresh `CreateRenderTarget` per Compile and
no cross-frame reuse; camera targets were allocated per resize through an entt
resource cache. No pooling existed anywhere. Presentation was implicit: a
`PresentOutput` pass flag made the Vulkan backend blit the first color
attachment full-screen into the swapchain with no viewport control, and the
`FrameOutput` usage flag functionally duplicated `TransferSrc`.

## Decision

1. **Builder/singleton split.** The per-frame graph object is renamed
   `RenderGraphBuilder` (the UE `FRDGBuilder` analogue). A module-level
   `RenderGraph` singleton owns the cross-frame facilities:
   `PipelineRegistry`, the new `RGRenderTargetPool`, and `Tick()/Shutdown()`.

2. **Renderer-driven transient view targets.** Cameras carry only a Viewport
   rectangle and lens parameters — no RHI resources. Renderers call
   `builder.CreateTexture({Width, Height, Format})` inside `Render()` for each
   view target they need; graph-created transients are acquired from
   `RGRenderTargetPool`, keyed by the full desc hash (format + extent + derived
   usage). Persistent cross-frame state (accumulation, future TAA history)
   stays renderer-owned and is Imported per frame. The transient/persistent
   split criterion is whether the content carries information across frames,
   not logical ownership.

3. **Derived usage.** Render-target usage is computed by Compile after pruning
   as the union over surviving views: `RGColorRT`→`RenderTarget`,
   `RGDepthRT`→`DepthStencil`, `RGTextureSRV`→`ShaderResource`,
   `RGStorageTextureUAV`→`ShaderStorage`,
   `RGCopySrc`/`RGCopyDst`→`TransferSrc`/`TransferDst`. `FrameOutput` is
   deleted; `TransferDst` is retained for derivable completeness; the ignored
   `RHISampledTextureDesc.Usage` field is removed (sampled textures have fixed
   backend semantics). `RGTextureDesc` drops its `Allow*` usage-hint fields.

4. **Pool lifecycle.** Pool return is immediate: when a replaced RenderResult's
   pass list is destroyed, its transient refs return to the free list with no
   GPU-completion gate. Safety rests on two invariants: single-queue submission
   ordering (frame N+1's commands execute after frame N's on the queue) and
   per-frame image state initialization (no cross-frame layout assumptions).
   Eviction is LRU by frame age: `RenderGraph::Tick()` runs in RenderLoop
   immediately before `IRenderer::Render()` and evicts free-list entries idle
   for 10 consecutive frames. `RenderGraph::Shutdown()` clears the pool and
   `PipelineRegistry` before `RHIRenderDevice::Destroy()`.

5. **Explicit present.** Presentation is a graph-visible command:
   `RHIBlitToSwapchainCmd{Source, DstRect}` (filter fixed to Nearest), issued
   by a `NeverPrune` `BlitToSwapchainPass` on a transfer pass. The complete
   source target is always blitted; no source sub-rectangle is exposed. The
   backend visitor resolves the currently acquired swapchain image, which
   remains backend-private and is never exposed as an `RHIRef`. The destination
   rect comes from the camera's Viewport; multiple views blit into distinct
   rects. `SetPresentOutput`, `RGColorRT.Present`,
   `TPass::PresentOutput`, and the backend's implicit present blit are deleted.

## Considered alternatives

- **Camera-owned targets with renderer-declared specs** — rejected: simulates
  usage derivation by hand without graph knowledge, and resize/renderer
  switching reintroduce cache plumbing.
- **Usage union at creation (status quo)** — rejected: couples Scene to every
  renderer's needs and over-allocates (storage flags paid by raster-only runs).
- **Pool inside the RHI backend** — rejected: the backend cannot know when a
  resource is idle; lifecycle hints would leak graph concerns back into RHI.
- **Exposing the swapchain image as an importable RHIRef** — rejected: breaks
  the backend-private swapchain invariant and forces graph-level layout
  tracking of acquired images.
- **Imported backbuffer as the present side effect (UE RDG style)** — rejected:
  UE keeps the pass alive because the backbuffer is an external graph resource;
  ours is backend-private, so `NeverPrune` is the correct equivalent.

## Consequences

- Scene holds no view render targets; the "camera-owned targets" exception in
  the Scene context is removed. `GBuffer` survives as a Renderer-module
  structure, not a Scene domain concept. Camera/Viewport migration details live
  in module CONTEXT.md files, not in this ADR.
- One pool serves all views and the editor; the entt resource caches for
  camera/editor targets are deleted.
- Usage varies frame to frame (e.g. picking adds `TransferSrc`); that is bucket
  churn in the pool, not an error.
- A second backend must implement the blit-to-swapchain command visitor and
  honor the two pool-safety invariants.
