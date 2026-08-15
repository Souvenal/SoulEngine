# Context: Renderer

**Namespace:** SoulEngine

Renderer implementations convert renderer-neutral SceneSnapshot input into a
move-only RHICommandList. Current renderers are RasterRenderer and
RayTracingRenderer; the selected renderer is cached by the engine and recorded
into each FrameSlot.

## Terms

| Term | Definition |
|------|------------|
| **IRenderer** | Renderer interface invoked by RenderLoop with one SceneSnapshot; returns a RenderResult containing a command list. |
| **RenderResult** | Per-frame RHI packet. It owns the command list only until RHILoop moves it into RHIRenderDevice::Execute(). |
| **Renderer cache** | Engine-global cache of attached renderers. Renderer-local request/resource wrappers may survive renderer switching until engine shutdown. |
| **Recorded resource ref** | An RHIRef<T> whose `operator bool()` confirms a Ready payload before recording. The command copies the ref as the GPU-use lifetime carrier. |
| **GBuffer** | Camera-owned collection of four color render targets plus one shared depth render target: albedo, normal, material ID, entity ID, and depth. |
| **ViewRenderTargets** | Camera-owned view output bundle containing the GBuffer and the final SceneColorRT used as the present source. |
| **Geometry pass** | Raster RHIPass that writes the four G-buffer color attachments and the shared depth attachment. |
| **Lighting pass** | Separate raster RHIPass that samples the G-buffer, including shared depth, and writes SceneColorRT. |
| **Present source** | Ref-backed final engine-owned SceneColorRT assigned to RHICommandList::PresentSourceRef; it is presented by the backend, not rendered directly into the swapchain by Renderer. |

## Relationships

- Application owns mutable Scene state. GameLoop makes snapshots and captures
  the selected shared renderer into a FrameSlot.
- RenderLoop calls IRenderer::Render(), stores the result, and publishes
  RenderReady; it does not make Vulkan calls.
- Renderer requests textures, meshes, buffers, pipelines, samplers, BLAS, and
  TLAS through ResourceManager. Native creation is queued to the RHI thread.
- A renderer records only ready refs. The command list copies refs for pass
  attachments, pipelines, vertex/index buffers, shader parameter resources,
  TLAS/BLAS instances, and the present source. Do not store a raw pointer
  obtained from TryGet() in a command.
- Vulkan retains the submitted command list through its graphics timeline. A
  renderer cache release or Resource transient collection after submission
  cannot destroy an ordinary ref-backed resource still visible to the GPU.
- RasterRenderer owns the raster GBuffer/deferred-lighting path, records separate Geometry and Lighting RHIPass instances, and assigns SceneColorRT as the present source.
  RayTracingRenderer uses ref-backed TLAS/BLAS, output, accumulation targets,
  and transient geometry/material/view buffers; Renderer never observes a
  Vulkan device address or descriptor index.

## Dependencies

- Core — logging, config
- RHI — command list, ref-backed payload types, transient handles
- Scene — immutable per-frame input
- Resource — logical resource requests and ready wrappers
- Shader / Material — reflection-derived parameter layouts and values