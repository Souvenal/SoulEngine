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
| **CameraRenderTargets** | Camera-owned view output bundle containing the GBuffer and the final SceneColorRT used as the present source. |
| **Geometry pass** | Raster RHIPass that writes the four G-buffer color attachments and the shared depth attachment. |
| **Lighting pass** | Separate raster RHIPass that samples the G-buffer, including shared depth, and writes SceneColorRT. |
| **Post-process pass** | A raster RHIPass appended after scene lighting. Post-process builders live under `Renderer/PostProcess/` and operate on the current view's ref-backed targets. |
| **EditorViewRecord** | Editor-only view snapshot carrying one editor camera pose, its persistent camera render targets, and the R8_UNORM selection mask used by editor post-processing. |
| **Editor selection outline** | Editor post-process that reads the EntityId G-buffer at the selected pixel, then marks pixels adjacent to the selected ID with the orange outline color while preserving SceneColorRT for non-outline pixels. RasterRenderer consumes the editor-camera mask; it does not create one per frame. |
| **Present source** | Ref-backed final engine-owned SceneColorRT assigned to RHICommandList::PresentSourceRef; it is presented by the backend, not rendered directly into the swapchain by Renderer. |
| **GeometryRecordTable** | Slang facade (Common/Geometry.slang) over the per-pass geometry record table. Shader parameter blocks embed it instead of a raw `StructuredBuffer<GeometryRecord>`; it exposes `pullVertex(recordIndex, vertexIndex)` and `pullBoundingSphere(recordIndex)` and fully encapsulates the GPU-address ABI of GeometryRecord. Reflection recurses into such resource-facade structs, so host code binds the inner member path (e.g. `g_rasterDraw.geometryTable.records`), not the facade itself. |
| **VertexInfo** | Decoded vertex attributes (position, normal, tangent, uv) returned by `GeometryRecordTable::pullVertex`. Null tangent/texCoord addresses pull silent defaults ((1,0,0,1) and (0,0)). |

## Relationships

- Application owns mutable Scene state. GameLoop makes snapshots and captures
  the selected shared renderer into a FrameSlot.
- RenderLoop calls IRenderer::Render(), stores the result, and publishes
  RenderReady; it does not make Vulkan calls.
- Renderer requests textures, pipelines, samplers, BLAS, and TLAS through
  ResourceManager. MeshSystem owns MeshRecord and GeometryRecord loading.
  Native creation is queued to the RHI thread.
- RayTracingRenderer converts Scene-owned GeometryRecord values into RHI
  triangle geometry descriptions before requesting a BLAS; Resource does not
  depend on Scene mesh types.
- Renderer resolves each draw's material through MaterialManager (scene instance
  -> mesh-imported asset instance -> built-in default) and PbrMaterialResolver
  deduplicates GPU entries by (instance ID, HasUV0, HasTangents).
- A renderer records only ready persistent refs. Transient refs may remain
  pending until the RHI frame task completes; the command visitor owns the
  Ready/Failed validation. The command list copies refs for pass attachments,
  pipelines, vertex/index buffers, shader parameter resources, transient
  buffers, TLAS/BLAS instances, and the present source. Do not store a raw
  pointer obtained from TryGet() in a command.
- Vulkan retains the submitted command list through its graphics timeline. A
  renderer cache release or Resource transient collection after submission
  cannot destroy an ordinary ref-backed resource still visible to the GPU.
- Renderer creates current-frame transient data through typed
  `CreateTransientXXXBuffer` calls. The descriptors contain byte spans that are
  copied by the RHI creation path; renderer code never captures borrowed Scene
  records or stores upload work on individual scopes.
- Uploadable records own their `GpuData` ABI mirror and `BuildGpuData()`
  method. Renderer passes the resulting byte snapshot to a transient RHI
  creation descriptor and never queries a native Vulkan buffer object.
- RasterRenderer owns the raster GBuffer/deferred-lighting path, records separate Geometry and Lighting RHIPass instances, appends editor post-process passes when the snapshot carries selection input, and assigns SceneColorRT as the present source. LightRecord::GpuData is the shared 48-byte light ABI uploaded by Raster and RayTracing; light resources are bound only by lighting passes.
  RayTracingRenderer uses ref-backed TLAS/BLAS, output, accumulation targets,
  and transient geometry/material/view buffers; Renderer never observes a
  Vulkan device address or descriptor index.
  RasterRenderer consumes Geometry-granularity InstanceRecord values and
  resolves them into GPU-ABI geometry and instance tables plus indirect commands for vertex pulling. Geometry/instance
  data is published as frame-affined transient resource creation tasks before
  scope recording and publishes the required shader-read and
  indirect-command-read visibility.

## Dependencies

- Core — logging, config
- RHI — command list, ref-backed payload types, transient handles
- Scene — immutable per-frame input
- Resource — logical resource requests and ready wrappers
- Shader / Material — reflection-derived parameter layouts and values
