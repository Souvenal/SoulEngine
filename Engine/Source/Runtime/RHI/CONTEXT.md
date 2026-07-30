# Context: RHI

**Namespace:** `SoulEngine`

Render Hardware Interface — abstract GPU layer. Backends register via self-registering factory pattern.

## Terms

| Term | Definition |
|------|------------|
| **CommandList** | Data struct with `std::vector<Pass>` + optional `PresentSource` render target. Produced by `Renderer::Render()`, consumed by `RenderDevice::Execute()`. Pipeline binding explicit; draw commands repeat expected pipeline pointer for backend validation + draw-parameter lowering needing pipeline layout info. |
| **Pass** | One rendering scope with `RenderingDesc` + commands. Backend auto-wraps begin/end rendering. Pass may contain multiple pipelines — models attachment/rendering scope, not pipeline scope. |
| **Shader parameter layout** | Immutable pipeline reflection grouped into descriptor-set layouts. Maps shader parameter paths to typed binding requirements without exposing Vulkan binding numbers to Renderer. |
| **Shader parameters** | Copyable CPU-side snapshot of values by shader parameter path. Auto-partitioned into reflected shader parameter sets, recorded via `BindShaderParametersCmd`. |
| **Shader parameter set** | One partition of shader parameters matching exactly one reflected descriptor-set layout. Backend may associate with native descriptor set. |
| **Transient buffers and arenas** | RenderDevice allocates typed logical transient uniform/shader-storage buffer handles. Renderers write byte snapshots into command lists; RHI thread resolves each handle to range in backend-owned host-visible, host-coherent arena for current frame slot. |
| **Resource usage tracking** | RHI-command-level enumeration of GPU resources referenced by command list submission. |
| **RenderDevice** | Abstract interface for device management, resource creation, GPU lifecycle. `BindWindowSystem(IWindowSystem&)` pure virtual — binds main presentation window after backend construction. Process-wide singleton: `RHIRenderDevice::Create()` constructs backend, `RHIRenderDevice::Get().BindWindowSystem(WindowSys)` prepares presentation, `RHIRenderDevice::Get()` accesses, `RHIRenderDevice::Destroy()` tears down. Frame submission via `Execute(CommandList)`. |
| **VertexBuffer** | Runtime polymorphic base in `SoulEngine`. Immutable after creation. `CreateVertexBuffer` returns `VertexBufferCreateResult` struct (`UPtr` buffer + upload completion token) — Resource layer owns payload, tracks staging→device copy completion. |
| **IndexBuffer** | Same as VertexBuffer for index data. `CreateIndexBuffer` returns `IndexBufferCreateResult`. |
| **SampledTexture** | Shader-readable texture from CPU pixel data via dedicated transfer upload path. Creation returns unique `SampledTexture` payload + transfer upload completion token. Public RHI APIs use this name, not generic `Texture`. |
| **Resource array** | Mutable non-global `ResourceArray<T>` shader value — ordered array of resource observers. Resource-layer arrays retain `ResourceRef<T>` owners; RHI snapshots contain only resolved observers. Distinct from process-wide bindless registry. |
| **Sampler** | Shader-readable sampling state object. Resource-managed RHI payload bound by shader binding name — not Vulkan set/binding handle, not immutable pipeline-layout policy. |
| **Render target** | GPU-owned attachment image for color/depth rendering destination. Created via render-target-specific APIs as unique payload, not sampled texture asset loading. |
| **Swapchain image** | Backend-private presentation image from window surface. Not exposed as Resource-managed texture; final presentation copies/blits/resolves/renders engine-owned output into it via RHI/RenderGraph flow. Vulkan blits `CommandList::PresentSource` into acquired swapchain image. |
| **GraphicsPipeline** | Empty polymorphic base for graphics pipeline resources. Same pattern as VertexBuffer/IndexBuffer — backend casts down. |
| **BufferUsage** | Bitmask enum for buffer creation hints. Not a type — backend uses for VkBufferUsageFlags at allocation. |
| **Format, BufferUsage etc.** | Enums + trivial descriptor structs in `SoulEngine`. |
| **ResourceState** | Per-resource GPU state for barrier tracking. Backend maintains implicit last-known state per handle. |
| **GraphicsProgram** | Shader artifact consumed by graphics pipeline descriptors. Owns selected stage programs + pipeline-level reflection for linked shader combination. Refers to `ShaderGraphicsProgram`. |
| **Pipeline reflection** | Backend-agnostic shader-visible resource interface for one linked graphics pipeline shader combination. Records shader binding names + set/binding/type metadata in `ShaderReflection`, derives public shader parameter layout. Backend-native pipeline-layout objects stay backend-private. |
| **Shader binding name** | Reflected host-side lookup name for shader resource binding inside one pipeline layout. Public RHI callers bind/update resources by shader intent, never by Vulkan set/binding. Distinct from Resource names and Resource keys. |
| **Draw shader binding** | Draw-scope association from shader binding name to typed RHI resource observer or transient constant-buffer handle. Backends resolve via draw command's expected graphics pipeline reflection. |
| **Push constant command** | Explicit command-list write of ordinary shader data into currently bound graphics pipeline's reflected push-constant range. Renderer uses for tiny high-frequency values (texture/material indices); not a resource binding, does not name Vulkan set/binding numbers. |
| **Vertex input layout** | Explicit CPU-side description of how up to four vertex-buffer bindings map onto shader's reflected vertex input interface. Each binding: slot + stride; each attribute: location, source binding, format, offset. Reflection only for validation warnings. |
| **GPU completion token** | Public opaque RHI type for completion condition of submitted GPU work. Resource code may hold + query via RHI, but must not define token or interpret backend-specific timeline/fence/sync details. |
| **Immediate completion token** | GPU completion token from queue-qualified immediate task. Async resources publish ready only after final transfer/graphics dependency chain completes. |
| **BackendFactory** | `Core::Factory<RenderDevice>` — singleton-backed registry in `RHI:RenderDevice`. Backends self-register via `AutoRegistrar`. |

## Architecture

Module defines abstract interfaces (`RenderDevice`, `CommandList`) + process-wide singleton via `RHIRenderDevice::Get()`. `RHIRenderDevice::Create()` selects backend via `BackendFactory::Get().Create(name)`, stores result; `BindWindowSystem(IWindowSystem&)` prepares main presentation target.

Backends standalone modules (`export module Vulkan;`) compiled into same `.dylib`. Each registers at static-init via namespace-scope `BackendFactory::AutoRegistrar<ConcreteBackend>` — factory never imports backends. Adding new backend requires zero changes to `RHI.cppm`.

Consumers never see backend types directly. All interaction via `RenderDevice::Get()`.

## Singleton Lifecycle

```
EngineLoop::Init()
  ├── CreateWindowSystem()
  ├── RenderDevice::Create()          // backend factory + singleton store
  ├── RenderDevice::BindWindowSystem(WindowSys)
  └── SwitchApplication()
        └── App->OnAttach()           // renderer creates resources via RenderDevice::Get()

EngineLoop::Shutdown()
  ├── SignalFatalError()             // wake all threads
  ├── join RenderLoop / RHILoop
  ├── App->OnDetach()                // renderer releases SPtrs
  ├── clear FrameSlot.RenderPacket   // release command observer pointers
  ├── ResourceManager::Clear()       // release Manager-owned payloads
  ├── RenderDevice::Destroy()        // teardown singleton (VMA)
  └── IWindowSystem::Shutdown()
```

## Relationships

- `GraphicsPipelineDesc` accepts `ShaderGraphicsProgram` directly; stage-combination validation deferred — define at RHI contract level before backend pipeline creation.
- Pipeline reflection produced by ShaderCompiler for linked graphics shader combination. `GraphicsPipeline` exposes only derived shader parameter layout; public RHI callers bind by shader parameter path, never Vulkan set/binding.
- Reflected binding paths identify shader parameters within one pipeline layout. Must not conflate with Resource cache keys or debug names: shader parameter snapshot connects path to RHI resource observer for command recording.
- Sampled textures + samplers assigned through **Shader parameters**. Constant-buffer bindings get command-list-scoped transient constant-buffer handle. Resource arrays via `SetResourceArray`; runtime sampled texture arrays use `ResourceArray<SampledTexture>`, individual slot selection in material/object data.
- Missing draw shader bindings = development warnings, not auto draw skips. Renderers responsible for dependency readiness + fallback policy before emitting draw.
- Reflected vertex input interface ≠ **Vertex input layout**; former states required attributes, latter states how vertex buffers feed them.
- Vertex input binding descriptions (stride, binding slot) not derived from shader reflection. `GraphicsPipelineDesc::VertexInputLayout` provides explicit CPU layout; Vulkan backend uses reflection only to warn about missing locations/format mismatches. Current support: up to four vertex bindings; per-instance input rate deferred.
- Bindless texture selection in **Draw material data**. Public RHI binds `ResourceArray<SampledTexture>` by shader binding name, expresses selected slot via small draw/material data — not Vulkan set/binding numbers.
- Push constants = explicit command-list writes scoped by currently bound graphics pipeline. Renderers decide when to push small material/object indices; backends only validate against reflected push-constant ranges + issue native command.
- `AllocateTransientConstantBuffer` / `AllocateTransientShaderStorageBuffer` allocate typed logical RHI handles. Don't create backend-native memory or expose offsets; `RHIPass`/`RHINonRenderingPass` record data snapshots via matching write commands, backend resolves handles to physical arena ranges.
- **Resource usage tracking** belongs to RHI command model. Backends consume tracked resource set after submission to publish completion tokens; "which resources does this reference?" not backend-private concept.
- `CommandList` does not own GPU resources. Any RHI resource pointer in command/attachment descriptor = observer pointer.
- `CommandList::PresentSource` = engine-owned final color output for frame. Not a pass command; not a swapchain image.
- `RHI` must not import `Resource` or use `ResourceHandle`. Producer keeps observer pointers valid via owning `ResourceRef<T>` values outliving command execution.
- `RHI` does not import any backend module — factory creates backends via registered creator lambdas.
- GPU completion tokens = RHI-owned completion points. Resource state owned by Resource layer; RHI only creates/checks tokens.
- `GpuCompletionToken` in public RHI type surface. Resource code may store while GPU-pending, but token identity/interpretation owned by RHI.
- ResourceManager polls GPU completion tokens from RHI thread; RHI resource handles don't lazily transition Resource state on read.
- Async Resource v1 uses immediate completion tokens for sampled texture + vertex/index buffer readiness. Resource may chain transfer work to graphics-lane bridge before ready.
- `SampledTexture`, render targets, swapchain images = distinct RHI concepts. `SampledTexture` creation upload-backed; render targets use separate attachment APIs; swapchain images stay backend-private.
- `Sampler` ≠ sampled texture image data. Texture resources = shader-readable image storage; sampler resources = small named sampling profile.

## Vulkan Backend Constraints

| Constraint | Detail |
|------------|--------|
| **Module layout** | Standalone `Vulkan` module in `Vulkan/` — self-registers with `RHIBackendFactory`. Internal partitions: `Vulkan:Types`, `Vulkan:RenderDevice`, `Vulkan:Command`, `Vulkan:Swapchain` |
| **Rendering** | Dynamic rendering (VK_KHR_dynamic_rendering / Vulkan 1.3) — no RenderPass objects |
| **vulkan-hpp** | Exceptions disabled (`VULKAN_HPP_NO_EXCEPTIONS`) |
| **Memory** | VMA (VulkanMemoryAllocator) for GPU memory |

## Thread Model

RHI resources not thread-safe by default. Callers must serialize:

| Resource | Thread safety |
|----------|---------------|
| `RenderDevice::Get()` | Safe from any thread (singleton) |
| `RenderDevice::Execute()` | `RHILoop` only |
| `CreateVertexBuffer` / `CreateIndexBuffer` / `CreateSampler` etc. | RHI-thread owned for backend-native object creation; historical `OnAttach` sync calls migrated to async ResourceManager requests |
| `RHIPass::WriteTransientConstantBuffer` | Called by render code before `Execute()`; backend consumes copied draw-scope write on `RHILoop` via dynamic uniform-buffer descriptor binding |
| `ImmediateContext` | Not thread-safe (caller serializes). Owns transfer + graphics lanes, each with queue-local timeline. Cross-lane waits = explicit task dependencies; `Tick()` retires callbacks from both lanes |

Backend-native RHI object creation, descriptor writes, GPU upload submission, GPU completion publication = RHI thread. Game/Render/background threads may prepare CPU-side request data or observe published handles/status, but must not mutate backend-native RHI state.

Frame pipeline (GameLoop / RenderLoop / RHILoop) managed by `SoulEngine::EngineLoop` — see [`Launch/CONTEXT.md`](../Launch/CONTEXT.md).

## Dependencies

- `Core` — logging, config, `Singleton`, `Factory`
- `Shader` — `ShaderGraphicsProgram` + compiled shader artifact types (consumes)
- `WindowSystem` — main-window binding + framebuffer extent
- Third-party: vulkansdk, VMA

## BDA ray-tracing geometry

Ray-tracing geometry metadata uses two per-dispatch transient storage buffers: instance rows map InstanceID() to geometry range, geometry rows contain source-buffer BDA + layout data. Renderer records logical source-buffer observers via WriteRayTracingGeometryDataCmd; Vulkan resolves device addresses during command execution. UsageVisitor stamps every referenced source buffer so normal GPU-completion deletion contract covers BDA reads.