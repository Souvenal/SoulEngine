# Context: RHI

**Namespace:** `SoulEngine::RHI`

Render Hardware Interface — abstract GPU abstraction layer with backends registered via a
self-registering factory pattern.

## Terms

| Term | Definition |
|------|------------|
| **CommandList** | Data struct containing `std::vector<Pass>` and `GlobalConstantData`. Produced by `Renderer::Render()`, consumed by `RenderDevice::Execute()`. Commands are `std::variant` types (`SetPipelineCmd`, `BindVertexBufferCmd`, etc.) carrying `SPtr<>` to RHI resources. |
| **Pass** | One rendering scope with `RenderingDesc` and commands inside. Backend auto-wraps in begin/end rendering. Pass has builder methods (`SetPipeline`, `BindVertexBuffer`, `DrawIndexed`). |
| **Draw material data** | Per-draw shader parameter payload emitted as an RHI command before a draw call. |
| **Resource usage tracking** | RHI-command-level enumeration of GPU resources referenced by a command list submission. |
| **RenderDevice** | Abstract interface for device management, resource creation (Create) and GPU lifecycle. `Init(GLFWwindow*)` is pure virtual — backends do setup there, not in the constructor. Process-wide singleton: `RenderDevice::Create(Window)` bootstraps, `RenderDevice::Get()` accesses, `RenderDevice::Destroy()` tears down. Frame submission via `Execute(CommandList)`. |
| **VertexBuffer** | Runtime polymorphic base in `SoulEngine::RHI`. Immutable after creation. Current `CreateVertexBuffer` returns a `VertexBufferCreateResult` struct (buffer + upload completion token) so the Resource layer can track when staging → device copies complete. |
| **IndexBuffer** | Same role as VertexBuffer, for index data. `CreateIndexBuffer` returns `IndexBufferCreateResult`. |
| **SampledTexture** | Shader-readable texture created from CPU pixel data through the dedicated transfer upload path. Its creation returns a `SampledTexture` payload plus a transfer upload completion token. Public RHI sampled-texture APIs use this name instead of the generic `Texture` name. |
| **Render target** | GPU-owned attachment image used as a color/depth rendering destination. It is created through render-target-specific APIs, not through sampled texture asset loading. |
| **Swapchain image** | Backend-private presentation image acquired from the window surface. It is not exposed as a Resource-managed texture; final presentation copies, blits, resolves, or renders engine-owned output into it through RHI/RenderGraph presentation flow. |
| **GraphicsPipeline** | Empty polymorphic base class for graphics pipeline resources. Same pattern as VertexBuffer/IndexBuffer — backend casts down. |
| **BufferUsage** | Bitmask enum for buffer creation hints. Not a type — backend uses it to decide VkBufferUsageFlags at allocation time. |
| **Format, BufferUsage etc.** | Enums and trivial descriptor structs in `SoulEngine::RHI`. |
| **ResourceState** | Per-resource GPU state for barrier tracking. Backend maintains implicit last-known state per handle. |
| **Program** | Shader artifact consumed directly by pipeline descriptors; RHI does not wrap it in an extra shader-stage descriptor when no RHI-only fields are needed. Refers to `Shader::Program`. |
| **PipelineResourceLayout** | Backend-agnostic description of the shader-visible resource interface for a pipeline, derived by merging per-shader reflection and used as the cache/share key for backend-native layout objects instead of exposing a Vulkan-specific pipeline layout type. |
| **Vertex input layout** | CPU/feed-side description of how one vertex buffer maps onto the shader's reflected vertex input interface. This layout is explicit and authoritative for stride/offset/format; shader reflection is used only for validation warnings. |
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
  ├── clear FrameSlot.CmdList        // release SPtrs in variant commands
  ├── RenderDevice::Destroy()        // teardown singleton (VMA)
  └── WindowDisplay::Shutdown()
```

## Relationships

- `GraphicsPipelineDesc` accepts `Shader::Program` directly; stage-combination validation is deferred and should be defined at the RHI contract level before backend pipeline creation.
- `PipelineResourceLayout` is derived inside the RHI/backend layer by merging normalized per-program shader reflection; it is not yet exposed as a public handle and should carry a TODO when implemented.
- A shader's reflected vertex input interface is a different concept from **Vertex input layout**; the former states what attributes are required, the latter states how application-side vertex buffers feed them.
- Vertex input binding descriptions (stride, binding slot, input rate) are not derived from shader reflection. `GraphicsPipelineDesc::VertexInputLayout` provides the explicit CPU layout, while the Vulkan backend uses reflection only to warn about missing locations or format mismatches. Extension to multi-binding / instance-rate is deferred.
- Bindless texture selection belongs to **Draw material data**. The public RHI command surface should express draw/material payloads, not backend descriptor-slot mutation commands.
- **Resource usage tracking** belongs to the RHI command model. Backends consume the tracked resource set after successful submission to publish completion tokens, but the question "which resources does this command reference?" is not a backend-private concept.
- `RHI` does not import any backend module — the factory creates backends via registered creator lambdas.
- GPU completion tokens are RHI-owned completion points. Resource state remains owned by the Resource layer; RHI only creates and checks completion tokens.
- `GpuCompletionToken` belongs to the public RHI type surface. Resource code may store it while a resource is GPU-pending, but token identity and interpretation remain owned by RHI.
- ResourceManager polls GPU completion tokens explicitly from the RHI thread; RHI resource handles do not lazily transition Resource state on read.
- Async Resource v1 uses transfer upload completion for sampled texture and vertex/index buffer readiness. The token is produced by backend transfer upload submission and queried non-blockingly before a resource moves from GPU-pending to ready.
- `SampledTexture`, render targets, and swapchain images are distinct RHI concepts. `SampledTexture` creation is upload-backed; render targets use separate attachment creation APIs; swapchain images remain backend-private presentation targets.

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
| `CreateVertexBuffer` / `CreateIndexBuffer` etc. | RHI-thread owned for backend-native object creation; historical `OnAttach` synchronous calls migrated to async Resource::Manager requests |
| `WriteGlobalConstantBuffer` | Called from `RHILoop` via `Execute()` |
| `ImmediateContext` | Not thread-safe (caller must serialize) |

Backend-native RHI object creation, descriptor writes, GPU upload submission, and GPU completion publication belong to the RHI thread. Game, Render, and background worker threads may prepare CPU-side request data or observe published handles/status, but must not directly mutate backend-native RHI state.

The frame pipeline (GameLoop / RenderLoop / RHILoop) is managed by `SoulEngine::Launch::EngineLoop` — see [`Launch/CONTEXT.md`](../Launch/CONTEXT.md).

## Dependencies

- `Core` — logging, config, `Singleton`, `Factory`
- `Shader` — `Shader::Program` and compiled shader artifact types (consumes)
- Third-party: vulkansdk, VMA
