# Context: RHI

**Namespace:** `SoulEngine::RHI`

Render Hardware Interface — abstract GPU abstraction layer with backends registered via a
self-registering factory pattern.

## Terms

| Term | Definition |
|------|------------|
| **CommandList** | Data struct containing `std::vector<Pass>` and an optional `PresentSource` render target. Produced by `Renderer::Render()`, consumed by `RenderDevice::Execute()`. Pipeline binding is explicit, while draw commands repeat the expected pipeline pointer for backend validation and draw-parameter lowering that needs pipeline layout information. |
| **Pass** | One rendering scope with `RenderingDesc` and commands inside. Backend auto-wraps in begin/end rendering. A pass may contain multiple pipelines because the pass models attachment/rendering scope, not pipeline scope. |
| **Shader parameter layout** | Immutable pipeline reflection grouped into descriptor-set layouts. It maps shader parameter paths to typed binding requirements without exposing Vulkan binding numbers to Renderer. |
| **Shader parameters** | Copyable CPU-side snapshot of values assigned by shader parameter path. It is automatically partitioned into reflected shader parameter sets and recorded through `BindShaderParametersCmd`. |
| **Shader parameter set** | One partition of shader parameters matching exactly one reflected descriptor-set layout. Backend realization may associate it with a native descriptor set. |
| **Resource usage tracking** | RHI-command-level enumeration of GPU resources referenced by a command list submission. |
| **RenderDevice** | Abstract interface for device management, resource creation (Create) and GPU lifecycle. `Init(GLFWwindow*)` is pure virtual — backends do setup there, not in the constructor. Process-wide singleton: `RenderDevice::Create(Window)` bootstraps, `RenderDevice::Get()` accesses, `RenderDevice::Destroy()` tears down. Frame submission via `Execute(CommandList)`. |
| **VertexBuffer** | Runtime polymorphic base in `SoulEngine::RHI`. Immutable after creation. `CreateVertexBuffer` returns a `VertexBufferCreateResult` struct (`UPtr` buffer + upload completion token) so the Resource layer can own the payload and track when staging → device copies complete. |
| **IndexBuffer** | Same role as VertexBuffer, for index data. `CreateIndexBuffer` returns `IndexBufferCreateResult`. |
| **SampledTexture** | Shader-readable texture created from CPU pixel data through the dedicated transfer upload path. Its creation returns a unique `SampledTexture` payload plus a transfer upload completion token. Public RHI sampled-texture APIs use this name instead of the generic `Texture` name. |
| **Resource array** | Mutable non-global `ResourceArray<T>` shader value representing one ordered array of resource observers. Resource-layer arrays retain matching `ResourceRef<T>` owners; RHI snapshots contain only resolved observers. It is distinct from a process-wide bindless registry. |
| **Sampler** | Shader-readable sampling state object. It is a Resource-managed RHI payload bound by shader binding name, not a Vulkan set/binding handle and not immutable pipeline-layout policy. |
| **Render target** | GPU-owned attachment image used as a color/depth rendering destination. It is created through render-target-specific APIs as a unique payload, not through sampled texture asset loading. |
| **Swapchain image** | Backend-private presentation image acquired from the window surface. It is not exposed as a Resource-managed texture; final presentation copies, blits, resolves, or renders engine-owned output into it through RHI/RenderGraph presentation flow. Current Vulkan presentation blits `CommandList::PresentSource` into the acquired swapchain image. |
| **GraphicsPipeline** | Empty polymorphic base class for graphics pipeline resources. Same pattern as VertexBuffer/IndexBuffer — backend casts down. |
| **BufferUsage** | Bitmask enum for buffer creation hints. Not a type — backend uses it to decide VkBufferUsageFlags at allocation time. |
| **Format, BufferUsage etc.** | Enums and trivial descriptor structs in `SoulEngine::RHI`. |
| **ResourceState** | Per-resource GPU state for barrier tracking. Backend maintains implicit last-known state per handle. |
| **GraphicsProgram** | Shader artifact consumed directly by graphics pipeline descriptors. It owns the selected stage programs plus pipeline-level reflection for the linked shader combination. Refers to `Shader::GraphicsProgram`. |
| **Pipeline reflection** | Backend-agnostic shader-visible resource interface for one linked graphics pipeline shader combination. It records shader binding names plus set/binding/type metadata in `Shader::Reflection` and derives the public shader parameter layout. Backend-native pipeline-layout objects remain backend-private. |
| **Shader binding name** | Reflected host-side lookup name for a shader resource binding inside one pipeline layout. Public RHI callers may use it to bind or update resources by shader intent, never by Vulkan set/binding. It is distinct from Resource names and Resource keys. |
| **Draw shader binding** | Draw-scope association from a shader binding name to a typed RHI resource observer, such as a sampled texture, sampler, or constant buffer. Backends resolve it through the draw command's expected graphics pipeline reflection. |
| **Push constant command** | Explicit command-list write of ordinary shader data into the currently bound graphics pipeline's reflected push-constant range. Renderer code uses it for tiny high-frequency values such as texture/material indices; it is not a resource binding and does not name Vulkan set/binding numbers. |
| **Vertex input layout** | Explicit CPU/feed-side description of how up to four vertex-buffer bindings map onto the shader's reflected vertex input interface. Each binding supplies a slot and stride; each attribute supplies a location, source binding, format, and offset. Reflection is used only for validation warnings. |
| **GPU completion token** | Public opaque RHI type representing the completion condition for submitted GPU work. Resource code may hold the token and query completion through RHI, but must not define the token or interpret backend-specific timeline, fence, or sync-object details. |
| **Transfer upload completion** | GPU completion token produced by backend upload work submitted through the dedicated transfer path. Async Resource v1 uses this for sampled texture and buffer upload readiness; graphics pipelines normally do not produce one. |
| **BackendFactory** | `Core::Factory<RenderDevice>` — singleton-backed registry defined in `RHI:RenderDevice`. Backends self-register via `AutoRegistrar`. |

## Architecture

The module defines abstract interfaces (`RenderDevice`, `CommandList`) and a process-wide singleton
accessed via `RenderDevice::Get()`.  `RenderDevice::Create(GLFWwindow*)` selects a backend via
`BackendFactory::Get().Create(name)` and stores the result.

Backends are standalone modules (`export module Vulkan;`) compiled into the same `.dylib`.
Each backend registers itself at static-init time via a namespace-scope
`BackendFactory::AutoRegistrar<ConcreteBackend>` — the factory facade never imports backends.
Adding a new backend requires zero changes to `RHI.cppm`.

Consumers of the RHI module never see backend types directly. All interaction goes through
`RenderDevice::Get()`.

## Singleton Lifecycle

```
EngineLoop::Init()
  ├── WindowDisplay::Create()
  ├── RenderDevice::Create(Window)   // bootstrap — factory + Init + store
  └── SwitchApplication()
        └── App->OnAttach()           // renderer creates resources via RenderDevice::Get()

EngineLoop::Shutdown()
  ├── SignalFatalError()             // wake all threads
  ├── join RenderLoop / RHILoop
  ├── App->OnDetach()                // renderer releases SPtrs
  ├── clear FrameSlot.RenderPacket   // release command observer pointers
  ├── ResourceManager::Clear()       // release Manager-owned payloads
  ├── RenderDevice::Destroy()        // teardown singleton (VMA)
  └── WindowDisplay::Shutdown()
```

## Relationships

- `GraphicsPipelineDesc` accepts `Shader::GraphicsProgram` directly; stage-combination validation is deferred and should be defined at the RHI contract level before backend pipeline creation.
- Pipeline reflection is produced by ShaderCompiler for the linked graphics shader combination. `GraphicsPipeline` exposes only its derived shader parameter layout; public RHI callers bind values by shader parameter path, never by Vulkan set/binding.
- Reflected binding paths identify shader parameters within one pipeline layout. They must not be conflated with Resource cache keys or debug names: a shader parameter snapshot connects a path to an RHI resource observer for command recording.
- Sampled textures, samplers, and constant buffers are assigned through **Shader parameters**. Resource arrays are assigned through `SetResourceArray`; runtime sampled texture arrays currently use `ResourceArray<SampledTexture>`, while individual slot selection belongs to material/object data.
- Missing draw shader bindings are development warnings, not automatic draw skips. Renderers remain responsible for dependency readiness and fallback policy before emitting a draw.
- A shader's reflected vertex input interface is a different concept from **Vertex input layout**; the former states what attributes are required, the latter states how application-side vertex buffers feed them.
- Vertex input binding descriptions (stride and binding slot) are not derived from shader reflection. `GraphicsPipelineDesc::VertexInputLayout` provides the explicit CPU layout, while the Vulkan backend uses reflection only to warn about missing locations or format mismatches. The current command and Vulkan lowering support up to four vertex bindings; per-instance input rate remains deferred.
- Bindless texture selection belongs to **Draw material data**. The public RHI command surface binds `ResourceArray<SampledTexture>` by shader binding name and expresses the selected slot through small draw/material data, not Vulkan set/binding numbers.
- Push constants are explicit command-list writes scoped by the currently bound graphics pipeline. Renderers decide when to push small material/object indices; backends only validate the write against reflected push-constant ranges and issue the native command.
- **Resource usage tracking** belongs to the RHI command model. Backends consume the tracked resource set after successful submission to publish completion tokens, but the question "which resources does this command reference?" is not a backend-private concept.
- `CommandList` does not own GPU resources. Any RHI resource pointer in a command or attachment descriptor is an observer pointer.
- `CommandList::PresentSource` is the engine-owned final color output for the frame. It is not a pass command and it must not be confused with a swapchain image.
- `RHI` must not import `Resource` or use `ResourceHandle`. The producer side is responsible for keeping observer pointers valid through owning `ResourceRef<T>` values that outlive command execution.
- `RHI` does not import any backend module — the factory creates backends via registered creator lambdas.
- GPU completion tokens are RHI-owned completion points. Resource state remains owned by the Resource layer; RHI only creates and checks completion tokens.
- `GpuCompletionToken` belongs to the public RHI type surface. Resource code may store it while a resource is GPU-pending, but token identity and interpretation remain owned by RHI.
- ResourceManager polls GPU completion tokens explicitly from the RHI thread; RHI resource handles do not lazily transition Resource state on read.
- Async Resource v1 uses transfer upload completion for sampled texture and vertex/index buffer readiness. The token is produced by backend transfer upload submission and queried non-blockingly before a resource moves from GPU-pending to ready.
- `SampledTexture`, render targets, and swapchain images are distinct RHI concepts. `SampledTexture` creation is upload-backed; render targets use separate attachment creation APIs; swapchain images remain backend-private presentation targets.
- `Sampler` is distinct from sampled texture image data. Texture resources provide shader-readable image storage; sampler resources provide a small named sampling profile.

## Vulkan Backend Constraints

| Constraint | Detail |
|------------|--------|
| **Module layout** | Standalone `Vulkan` module in `Vulkan/` — self-registers with `RHI::BackendFactory`. Internal partitions: `Vulkan:Types`, `Vulkan:RenderDevice`, `Vulkan:Command`, `Vulkan:Swapchain` |
| **Rendering** | Dynamic rendering (VK_KHR_dynamic_rendering / Vulkan 1.3) — no RenderPass objects |
| **vulkan-hpp** | With exceptions disabled (`VULKAN_HPP_NO_EXCEPTIONS`) |
| **Memory** | VMA (VulkanMemoryAllocator) for GPU memory management |

## Thread Model

RHI resources are not thread-safe by default. Callers must serialize access:

| Resource | Thread safety |
|----------|---------------|
| `RenderDevice::Get()` | Safe from any thread (singleton) |
| `RenderDevice::Execute()` | Called from `RHILoop` only |
| `CreateVertexBuffer` / `CreateIndexBuffer` / `CreateSampler` etc. | RHI-thread owned for backend-native object creation; historical `OnAttach` synchronous calls migrated to async Resource::Manager requests |
| `DrawParameter::WriteConstantBuffer` | Called by render code before `Execute()`; backend consumes the copied draw-scope writes on `RHILoop` through dynamic uniform-buffer descriptor binding. |
| `ImmediateContext` | Not thread-safe (caller must serialize). Its completion queue and timeline must be owned by the same Vulkan queue on which it submits. The transfer immediate context uses the transfer completion queue; one-shot graphics work such as BLAS builds uses a separate graphics completion queue and signals completion at `eAccelerationStructureBuildKHR`. |

Backend-native RHI object creation, descriptor writes, GPU upload submission, and GPU completion publication belong to the RHI thread. Game, Render, and background worker threads may prepare CPU-side request data or observe published handles/status, but must not directly mutate backend-native RHI state.

The frame pipeline (GameLoop / RenderLoop / RHILoop) is managed by `SoulEngine::Launch::EngineLoop` — see [`Launch/CONTEXT.md`](../Launch/CONTEXT.md).

## Dependencies

- `Core` — logging, config, `Singleton`, `Factory`
- `Shader` — `Shader::GraphicsProgram` and compiled shader artifact types (consumes)
- Third-party: vulkansdk, VMA

## BDA ray-tracing geometry

`RayTracingGeometryTable` is a RenderDevice-owned logical metadata table for
ray-tracing source attributes. Renderer records `RayTracingGeometryDesc`
observers and instance-relative geometry ranges through
`UpdateRayTracingGeometryTableCmd`; it must not resolve device addresses.
`UsageVisitor` stamps the table and every referenced source buffer so the
normal GPU-completion deletion contract covers BDA reads.
