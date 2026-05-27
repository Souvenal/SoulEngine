# Context: RHI

**Namespace:** `SoulEngine::RHI`

Render Hardware Interface — abstract GPU abstraction layer with backends registered via a
self-registering factory pattern.

## Terms

| Term | Definition |
|------|------------|
| **CommandList** | Abstract interface for recording GPU commands (draw, bind, barrier). |
| **RenderDevice** | Abstract interface for device management, resource creation (Create) and GPU lifecycle. `Init(GLFWwindow*)` is pure virtual — backends do setup there, not in the constructor. Process-wide singleton: `RenderDevice::Create(Window)` bootstraps, `RenderDevice::Get()` accesses, `RenderDevice::Destroy()` tears down. |
| **VertexBuffer** | Empty polymorphic base class in `SoulEngine::RHI`. No public methods — purely a type token for the `RenderDevice::CreateVertexBuffer` return type and `CommandList::BindVertexBuffer` parameter. Vendors cast it to their concrete type (e.g. `Vulkan::VertexBuffer`) internally. |
| **IndexBuffer** | Same role as VertexBuffer, for index data. `BindIndexBuffer` takes `SPtr<IndexBuffer>` instead of raw buffer + stride, ensuring type safety at the API boundary. |
| **Texture / Pipeline / Sampler** | Opaque handle types (`Uint64 Handle = 0`) in `SoulEngine::RHI`. Backend maintains an internal map from handle to native resource. POD — trivially copyable. |
| **BufferUsage** | Bitmask enum for buffer creation hints. Not a type — backend uses it to decide VkBufferUsageFlags at allocation time. |
| **Format, BufferUsage etc.** | Enums and trivial descriptor structs in `SoulEngine::RHI`. |
| **ResourceState** | Per-resource GPU state for barrier tracking. Backend maintains implicit last-known state per handle. |
| **Program** | Shader artifact consumed directly by pipeline descriptors; RHI does not wrap it in an extra shader-stage descriptor when no RHI-only fields are needed. Refers to `Shader::Program`. |
| **PipelineResourceLayout** | Backend-agnostic description of the shader-visible resource interface for a pipeline, derived by merging per-shader reflection and used as the cache/share key for backend-native layout objects instead of exposing a Vulkan-specific pipeline layout type. |
| **Vertex buffer stream layout** | Future CPU/feed-side description of how one or more vertex buffers map onto the shader's reflected vertex input interface; distinct from shader reflection and not part of `PipelineResourceLayout`. |
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
        └── App->OnAttach()           // renderer uses RenderDevice::Get()

EngineLoop::Shutdown()
  ├── RenderDevice::Get().WaitIdle()
  ├── App->OnDetach()                // renderer destroyed
  ├── RenderDevice::Destroy()       // teardown singleton
  └── WindowDisplay::Shutdown()
```

## Relationships

- `GraphicsPipelineDesc` accepts `Shader::Program` directly; stage-combination validation is deferred and should be defined at the RHI contract level before backend pipeline creation.
- `PipelineResourceLayout` is derived inside the RHI/backend layer by merging normalized per-program shader reflection; it is not yet exposed as a public handle and should carry a TODO when implemented.
- A shader's reflected vertex input interface is a different concept from a future **Vertex buffer stream layout**; the former states what attributes are required, the latter states how application-side vertex buffers feed them.
- Vertex input binding descriptions (stride, binding slot, input rate) are not part of shader reflection. The Vulkan backend computes them inside `GraphicsShaderStates::Create` using `CalculateStride` from per-attribute ValueType, with binding slot hardcoded to 0 (single interleaved). Extension to multi-binding / instance-rate is deferred.
- `RHI` does not import any backend module — the factory creates backends via registered creator lambdas.

## Vulkan Backend Constraints

| Constraint | Detail |
|------------|--------|
| **Module layout** | Standalone `Vulkan` module in `Vulkan/` — self-registers with `RHI::BackendFactory`. Internal partitions: `Vulkan:Types`, `Vulkan:RenderDevice`, `Vulkan:CommandList`, `Vulkan:Swapchain` |
| **Rendering** | Dynamic rendering (VK_KHR_dynamic_rendering / Vulkan 1.3) — no RenderPass objects |
| **vulkan-hpp** | With exceptions disabled (`VULKAN_HPP_NO_EXCEPTIONS`) |
| **Memory** | VMA (VulkanMemoryAllocator) for GPU memory management |

## Thread Model

| Term | Definition |
|------|------------|
| **Game Thread** | Runs game logic tick (`OnTick`), produces per-frame draw data, writes to frame slot. Also runs window event polling. |
| **Render Thread** | Dedicated thread consuming game-produced frame data. Owns RHI resources, records CommandBuffers, calls `vkQueueSubmit` and present. Runs one frame behind game thread (1-frame pipeline bubble). |
| **Frame Slot** | Ring buffer of N `FrameContext` structs indexed by `(frameCount % m_FramesInFlight)`. Game writes current frame's slot; Render reads previous frame's slot. No shared write access to the same slot — zero-lock path. |

## Dependencies

- `Core` — logging, config, `Singleton`, `Factory`
- `Shader` — `Shader::Program` and compiled shader artifact types (consumes)
- Third-party: vulkansdk, VMA
