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
| **RenderResult** | Per-frame RHI packet owned by the RHI module: pass list, pending BLAS/TLAS build refs, and the ImGui snapshot. RenderLoop produces it; RHILoop borrows it for RHIRenderDevice::Execute() and keeps it alive until GPU completion. |
| **Renderer cache** | Engine-global cache of attached renderers. Renderer-local request/resource wrappers may survive renderer switching until engine shutdown. |
| **Recorded resource ref** | An RHIRef<T> whose `operator bool()` confirms a Ready payload before recording. The command copies the ref as the GPU-use lifetime carrier. |
| **GBuffer** | Raster-renderer view-target set of four color render targets (albedo, normal, material ID, entity ID) plus one shared depth target, defined by the Renderer module (`GBuffer.cppm`) and created per view as pooled graph transients from the camera's Viewport (ADR 05). Not a Scene concept; RayTracing does not produce a GBuffer. |
| **View targets** | The per-view render targets a renderer creates via `RenderGraphBuilder::CreateTexture` from the view's Viewport: the GBuffer set and SceneColor for Raster, output/EntityId targets for RayTracing. Contents live only for the frame; cross-frame state (accumulation, future TAA history) is a renderer-owned persistent resource imported per frame. |
| **Geometry pass** | Raster RHIPass that writes the four G-buffer color attachments and the shared depth attachment. |
| **Lighting pass** | Separate raster RHIPass that samples the G-buffer, including shared depth, and writes SceneColorRT. |
| **Post-process pass** | A raster RHIPass appended after scene lighting. Post-process builders live under `Renderer/PostProcess/` and operate on the current view's ref-backed targets. |
| **EditorViewRecord** | Editor-only view snapshot carrying one editor camera pose and Viewport; its view targets (including the R8_UNORM selection mask used by editor post-processing) are pooled graph transients like any other view's. |
| **Editor selection outline** | Editor post-process that reads the EntityId G-buffer at the selected pixel, then marks pixels adjacent to the selected ID with the orange outline color while preserving SceneColorRT for non-outline pixels. RasterRenderer consumes the editor-camera mask; it does not create one per frame. |
| **Present** | A graph-visible `BlitToSwapchainPass` (NeverPrune transfer pass) that blits the view's final output into the camera's Viewport rect of the backend-private swapchain via `RHIBlitToSwapchainCmd` (ADR 05). Raster blits SceneColor; RayTracing blits its accumulation output directly and has no SceneColor. |
| **GeometryRecordTable** | Slang facade (Common/Geometry.slang) over the per-pass geometry record table. Shader parameter blocks embed it instead of a raw `StructuredBuffer<GeometryRecord>`; it exposes `pullVertex(recordIndex, vertexIndex)` and `pullBoundingSphere(recordIndex)` and fully encapsulates the GPU-address ABI of GeometryRecord. Reflection recurses into such resource-facade structs, so host code binds the inner member path (e.g. `g_rasterDraw.geometryTable.records`), not the facade itself. |
| **VertexInfo** | Decoded vertex attributes (position, normal, tangent, uv) returned by `GeometryRecordTable::pullVertex`. Null tangent/texCoord addresses pull silent defaults ((1,0,0,1) and (0,0)). |
| **Static BLAS** | The bottom-level acceleration structure owned by a Scene `GeometryRecord`: a hollow descriptor until built once from its position/index buffers at the frame-start AS phase, immutable for the record's lifetime (no update/refit; dynamic geometry is out of scope). Geometry ownership replaces any external key-based BLAS cache. |
| **Per-frame TLAS** | A hollow top-level structure created fresh each frame by RayTracingRenderer from the SceneGpuData-derived instance list ({BLAS ref, world transform} per slot). Desc slot order is the canonical TLAS instance order — the shader maps InstanceIndex() back to the sorted instance table, so no custom index exists. The device allocates storage and builds at the frame-start AS phase; the previous frame's TLAS retires through deferred deletion. |
| **Lazy upload** | Geometry RHI buffers (and BLAS) are not created at asset load; the renderer thread requests them on first encounter of a `GeometryRecord` whose `IsUploadRequested(NeedBLAS)` is false, and draws it once `IsReadyOnGpu(NeedBLAS)` is true. The record retains CPU vertex data until upload completes. |
| **GeometryUploader** | Per-renderer instance with sole authority to mutate a `GeometryRecord`'s GPU state through the single entry point `Upload(record, NeedBLAS)` (member, staged, idempotent): queues buffer creation (`BufferUploadRequested`) and, once buffers are Ready, creates the hollow BLAS descriptor (`BlasRequested`) and enters it into the pending build batch in the same step — each BLAS joins exactly once. `UploadBlas` is the ray-tracing validation anchor: a geometry without complete triangles is excluded exactly once (warned, `BlasRequested` set, no BLAS, never enqueued; raster unaffected). `DrainPendingBlasBuilds` drains the batch into `RenderResult`; the device builds it at frame start. Queries (`IsUploadRequested` / `IsReadyOnGpu`) are const members of the record; mutation is never a member operation. |
| **SceneGpuData** | Per-frame renderer-neutral GPU table bundle (Common) built by `Create` from the renderer-filtered InstanceRecord values and the full scene light list: instance/geometry/material/light GPU-ABI tables plus per-geometry IndexCount (sourced from the RHI index buffer) and BLAS-ref parallel columns. Lights take no filter or upload path and are baked once per frame, shared by every view. Instances are sorted by GeometryID as the canonical order; material slot 0 is always the default entry; geometry and material stay unbound — the material resolves through the instance record, never through the geometry. Upload and readiness gating are each renderer's own filter step ahead of `Create` (buffers for raster, buffers+BLAS for ray tracing); `Create` itself is pure table building. Raster derives indirect commands from it; ray tracing derives the TLAS instance list from it. |

## Relationships

- Application owns mutable Scene state. GameLoop makes snapshots and captures
  the selected shared renderer into a FrameSlot.
- RenderLoop calls IRenderer::Render(), stores the result, and publishes
  RenderReady; it does not make Vulkan calls.
- MeshSystem owns MeshRecord and GeometryRecord loading. GeometryUploader and
  PipelineRegistry request native RHI payloads directly; buffers and pipelines
  are created on the RHI thread (EnqueueResourceCreation), while hollow BLAS
  descriptors are created synchronously (no device calls) and built by the
  device at frame start.
- Each renderer filters the frame's instances through its own GeometryUploader
  gate (buffers for raster, buffers+BLAS for ray tracing) and builds the shared
  SceneGpuData tables from the accepted values. RayTracingRenderer additionally
  collects hollow BLAS descriptors plus a freshly created per-frame TLAS into
  `RenderResult` for the frame-start AS phase; the TLAS desc slot order is the
  canonical instance order (see Per-frame TLAS).
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
  renderer cache release after submission cannot destroy an ordinary
  ref-backed resource still visible to the GPU.
- Renderer creates current-frame transient data through typed
  `CreateTransientXXXBuffer` calls. The descriptors contain byte spans that are
  copied by the RHI creation path; renderer code never captures borrowed Scene
  records or stores upload work on individual scopes.
- Uploadable records own their `GpuData` ABI mirror and `BuildGpuData()`
  method. Renderer passes the resulting byte snapshot to a transient RHI
  creation descriptor and never queries a native Vulkan buffer object.
- RasterRenderer owns the raster GBuffer/deferred-lighting path, records separate Geometry and Lighting RHIPass instances, appends editor post-process passes when the snapshot carries selection input, and presents by appending a BlitToSwapchainPass for SceneColor. LightRecord::GpuData is the shared 48-byte light ABI uploaded by Raster and RayTracing; light resources are bound only by lighting passes.
  RayTracingRenderer uses ref-backed TLAS/BLAS, output, and renderer-owned
  accumulation targets (persistent, imported per frame), plus transient
  geometry/material/view buffers; it writes primary normals/entity IDs to
  pooled storage-image view targets and blits its accumulation output directly
  to the swapchain. Renderer never observes a Vulkan device address or
  descriptor index.
  RasterRenderer derives per-geometry indirect commands from the shared
  SceneGpuData tables for vertex pulling. Geometry/instance
  data is published as frame-affined transient resource creation tasks before
  scope recording and publishes the required shader-read and
  indirect-command-read visibility.

## Dependencies

- Core — logging, config
- RHI — command list, ref-backed payload types, transient handles
- Scene — immutable per-frame input
- Shader / Material — reflection-derived parameter layouts and values
