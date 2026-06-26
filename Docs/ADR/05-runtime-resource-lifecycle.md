# ADR 05 — Runtime Resource Lifecycle

## Status

Accepted (2026-06-27)

## Part 1 — Problem

Runtime resources such as sampled texture assets, vertex/index buffers,
graphics pipelines, and camera render targets have two different lifetimes:

- a CPU-visible RHI object lifetime, where render code may hold observer
  pointers while building and executing a command list;
- a backend-native GPU lifetime, where Vulkan handles and VMA allocations may
  still be in use after `RenderDevice::Execute()` has returned.

The engine also uses a three-thread frame pipeline:

- Game thread mutates application state and requests resources.
- Render thread consumes a `SceneSnapshot` and produces RHI command data.
- RHI thread performs backend-native work and submits command lists.

The resource design must therefore satisfy all of these constraints:

- The frame path must not block on file IO, image decode, shader compilation,
  GPU uploads, or pipeline creation.
- Backend-native RHI creation and GPU submission must remain owned by the RHI
  thread.
- `RHI` must not import `Resource`, know about `ResourceHandle`, or participate
  in resource-manager ownership rules.
- Command lists must be pure RHI data and may store only non-owning observer
  pointers.
- A resource handle must be a stable typed ticket, not an accidental keepalive.
- ResourceContext-owned payloads must not be reset while a command list still
  observes them.
- Vulkan handles must not be destroyed until the GPU has completed every
  submission that references them.

## Part 2 — Designs We Tried or Rejected

### Synchronous frame-path creation

The earliest shape was direct, synchronous creation such as loading a texture or
creating a pipeline when the renderer needed it.

That is simple, but it conflicts with the thread model. File IO, image decode,
shader compilation, GPU upload, descriptor publication, and pipeline creation
can all become hidden frame hitches. It also blurs which thread owns
backend-native RHI mutation.

The current design keeps frame-path consumption non-blocking. A renderer either
uses a ready resource, uses a coherent fallback, or skips dependent work for the
current frame.

### Handles owning shared slots

A previous `ResourceHandle<T>` carried shared ownership of its backing
`ResourceSlot<T>`. That made a handle convenient to query directly, but it also
made lifetime implicit: any copied handle could keep a slot alive outside the
ResourceManager's control.

This was rejected because the Resource layer must own slots, state transitions,
and ready payloads explicitly through `ResourceContext`. A handle now stores only
`Resource key + ResourceGeneration + T`. It can be copied freely, but it does not
keep a resource alive.

### Command lists owning resources with `SPtr`

A previous command-list model stored `SPtr` references to RHI resources. That
prevented dangling command pointers, but it made command lists accidental
owners. It also hid lifetime extension inside the RHI command payload and made
shutdown ordering harder to reason about.

This was rejected because `RHI::CommandList` should describe work, not own
resources. The RHI module also must not learn Resource concepts to understand
why a command owns or releases a payload.

The current command list stores raw RHI observer pointers. The producer side
must keep those observers valid through a frame resource scope.

### Public low-level `Manager::Pin`

A direct `Manager::Pin(handle)` API works mechanically, but it creates an easy
mistake: code can pin, extract a pointer, emit a command, and forget to move the
pin into the render packet that must survive until RHI execution.

The low-level pin primitive therefore remains private to `Resource::Manager`.
Render code uses `FrameResourceScope::Acquire(ref)` when it has an owner ref,
or `FrameResourceScope::Acquire(handle)` when consuming a copyable render
snapshot. `Acquire()` pins, stores the pin in the frame scope immediately, and
returns the raw RHI pointer for the command list.

### Wrapper payload structs and `std::optional`

Earlier resource payloads were shaped as separate structs such as texture,
buffer, and pipeline payload wrappers, and slots used an additional optional
payload layer.

That added boilerplate without expressing a real second state. The ready
payload is now unified as `Resource<RHI::T>` for explicitly supported RHI
families. It owns `UPtr<T> Object`; absence of a ready object is represented by
`nullptr`. Unsupported `Resource<T>`, `ResourceHandle<T>`, or
`ResourceSlot<T>` instantiations fail at compile time through the combination
of `ResourceTraits` and `ManagedRHIResourceTypes`.

### DeletionQueue without pins, or pins without DeletionQueue

These mechanisms solve different problems and cannot replace each other.

Pins protect CPU object lifetime from Render to RHI execution. They make sure a
raw pointer in a command list still points to a valid `RHI::T` C++ object when
`RenderDevice::Execute()` reads it.

Usage tokens and `DeletionQueue` protect backend-native GPU lifetime after RHI
submission. They make sure Vulkan handles and memory are destroyed only after
the GPU has completed the submissions that referenced them.

Using only one side is insufficient:

- Without pins, a command-list observer pointer may dangle before `Execute()`
  can stamp usage tokens.
- Without usage tokens and deferred deletion, a pin can be released after
  `Execute()` while the GPU is still executing the submitted work.

## Part 3 — Current Ownership Model

`ResourceContext` is the sole owner of resource entries, slots, and ready
payloads; `Resource::Manager` owns the context and exposes the public facade.
This split is intentional: Context answers lifecycle questions, while Manager
answers API-shape questions for the rest of Runtime.

`ResourceHandle<T>` stores:

- canonical resource key;
- `ResourceGeneration`;
- the template parameter `T`, which is the RHI payload family.

The handle is a stable typed ticket. It is not a `SPtr`, does not point to a
slot, and does not keep the payload alive.

The handle is not the only Resource-facing value. The design keeps these
concepts separate because each one answers a different question:

| Concept | Question it answers | Responsibility | Forbidden responsibility |
|---------|---------------------|----------------|--------------------------|
| `ResourceRef<T>` | Does some runtime system still want this resource? | Public move-only logical owner. It retains a non-owning observer for the creating `ResourceContext` so destruction can decrement logical demand without calling back through `Resource::Manager`. | Payload access, frame pinning, cache policy, or async publication. |
| `ResourceHandle<T>` | Which canonical key and generation is being referenced? | Passive typed async ticket. Copyable and snapshot-safe. Async work uses it to publish only to the generation it started from. | Slot ownership, logical ownership, destructor side effects, or Manager/Context callbacks. |
| Resource entry | What registry record exists for this canonical key? | ResourceContext-owned node with request coalescing metadata, logical ref count, lifetime policy, and cache/eviction metadata. Contains the slot for the current generation. | RHI payload state transitions or command-list observer safety. |
| `ResourceSlot<T>` | What is the current payload state for this entry generation? | Internal payload state machine with generation, state, ready payload, retired pinned payloads, error, pin count, and deferred payload release flags. | Logical ref count, cache policy, eviction budget metadata, or request coalescing. |
| `ResourcePin<T>` | Is the ready payload protected for the current frame packet? | Frame execution guard. Keeps ready payloads from being reset while command-list observer pointers may still be consumed by RHI. | Logical ownership or cache retention. |
| `FrameResourceScope` | Where are frame pins stored until RHI consumes command-list observers? | Pin-and-store API used by render code before writing raw RHI observer pointers into command lists. | Resource requests, cache policy, or async state publication. |
| `ResourceContext` | Where does lifecycle state live? | Concrete owner of resource families, entries, slots, ready payloads, logical ref counts, and GPU-pending queues. | Public Runtime facade or family-specific loading logic. |
| `Resource::Manager` | What Resource API should the rest of Runtime call? | Singleton facade for request, query, tick, clear, collect, and frame acquire support. It forwards lifecycle work to Context. | Slot internals, per-family loading implementation, or ownership hidden in handles. |
| Request partitions | How does one family prepare and publish work? | Key derivation, CPU preparation, RHI-thread creation/upload, and result publication for one resource family. | Registry ownership, logical ref counting, frame pin storage, or public owner semantics. |

`ResourceRef<T>` does not replace frame pins. It expresses whether a system
still wants a resource. `ResourcePin<T>` expresses
whether a frame command packet still needs the ready payload object to remain
valid.

The internal layering rule is strict:

- logical ownership belongs to `ResourceRef<T>` and the ResourceContext-owned
  resource entry;
- async identity belongs to `ResourceHandle<T>`;
- payload state and frame pin safety belong to `ResourceSlot<T>` and
  `ResourcePin<T>`;
- family registry ownership, ref counts, and GPU-pending queues belong to
  `ResourceContext`;
- family-specific loading belongs to request partitions, with shared request
  admission and stale/shutdown publish behavior in request-common helpers;
- public request APIs should expose `ResourceRef<T>`, not raw handles, slots,
  entries, or pins.

This prevents a single type from becoming both a cache registry node, an async
ticket, and a payload safety mechanism.

`ResourceContext` is the concrete owner of resource entries, slots, and ready
payloads. `Resource::Manager` is the public facade that keeps request, query,
tick, clear, collect, and frame-acquire entry points stable for the rest of
Runtime. Manager must stay thin: if an implementation needs entry maps, ref
counts, slot mutation, or GPU-pending queues, it belongs in Context; if it
needs texture decode, mesh import, shader compilation, or RHI object creation,
it belongs in a request partition.
`ResourceRef<T>` is a public type defined in its own Resource partition.
Manager creates valid refs only after `ResourceContext::AddRef()` accepts the
handle. The ref stores a non-owning pointer to the creating context and calls
`ResourceContext::ReleaseRef()` on reset/destruction, avoiding both Manager
callbacks and type-erased release callbacks. `ResourceHandle<T>` must remain
passive and must not call back into Manager or Context.

Supported payload families require two compile-time declarations:

- `ResourceTraits<RHI::T>::Info` describes GPU-pending behavior, diagnostics,
  and default lifetime policy.
- `ManagedRHIResourceTypes` lists the families that `ResourceContext` and
  `FrameResourceScope` allocate storage for.

`ManagedRHIResource` requires both declarations. A traits-only type is
intentionally rejected because it would have no Context family or frame-pin
storage.

Current managed families are:

- `RHI::SampledTexture`;
- `RHI::RenderTarget`;
- `RHI::GraphicsPipeline`;
- `RHI::VertexBuffer`;
- `RHI::IndexBuffer`.

The payload type is:

```cpp
template <ManagedRHIResource T>
struct Resource {
    UPtr<T> Object = nullptr;
};
```

Resource-managed RHI creation APIs return unique payloads. Vulkan backend
objects may still use `SPtr` internally to defer native handle destruction, but
that sharing is not exposed through Resource handles, frame scopes, or command
lists.

## Part 4 — Asynchronous Request Validity

Resource readiness means render-consumable readiness: CPU preparation, RHI
commitment, required GPU transfer work, and publication have completed.

The request pipeline is split by ownership:

- Background TaskGraph workers perform non-thread-affine CPU work such as file
  IO, image decode, shader compilation, and request data preparation.
- The RHI thread performs backend-native object creation, descriptor writes,
  upload submission, and completion publication.
- The Render thread resolves dependencies before emitting coherent command
  data.

Equivalent in-flight requests are coalesced by canonical resource key and return
handles to the same resource identity. Each slot has a generation. Async work
captures the generation it belongs to, and may publish only when that generation
still matches the current slot generation.

This makes stale completion safe. If an old request completes after a newer
generation has replaced the slot contents, the old completion is ignored and
the stale handle observes `ResourceState::Stale`.

The public states distinguish the major phases:

- `CpuPreparing`;
- `RhiCommitting`;
- `GpuPending`;
- `Ready`;
- `Failed`;
- `Stale`;
- `Unknown`.

`GetState(handle)` and `GetError(handle)` are typed Manager queries. They do
not keep payloads alive and do not lazily advance GPU completion. GPU-pending
resources are polled explicitly by the ResourceManager on the RHI thread.

When a resource is not ready on the runtime frame path, the default policy is to
skip dependent work. Fallback resources are allowed when a coherent substitute
exists. Blocking waits are reserved for startup, tooling, tests, or other
non-frame-path workflows.

## Part 5 — Frame-Scoped CPU Lifetime

`RHI::CommandList` stores only observer pointers:

- pipeline commands store `RHI::GraphicsPipeline*`;
- vertex/index buffer commands store `RHI::VertexBuffer*` and
  `RHI::IndexBuffer*`;
- material data stores `RHI::SampledTexture*`;
- attachment descriptors store `RHI::RenderTarget*`.

Those pointers are valid only because the render packet owns a
`Resource::FrameResourceScope`.

Renderer code resolves refs, or snapshot handles, like this:

```cpp
auto* Texture = Result.Resources.Acquire(TextureRef);
if (!Texture)
    return Result;

Pass.SetDrawMaterialData(RHI::DrawMaterialData{.TestTexture = Texture});
```

`FrameResourceScope::Acquire(ref)`:

1. obtains the ref's internal handle;
2. asks `Resource::Manager` to pin the handle;
3. succeeds only if the handle generation matches and the resource is `Ready`;
4. stores the RAII pin inside the frame scope immediately;
5. returns `T*` for the command list.

`FrameResourceScope::Acquire(handle)` follows the same pin-and-store sequence
for render snapshots that cannot carry move-only refs. The boundary is the
FrameScope API, not the argument type: render code must not call Manager's
low-level pin primitive directly.

The render packet is:

```cpp
struct RenderResult {
    RHI::CommandList             CmdList   = {};
    Resource::FrameResourceScope Resources = {};
};
```

`FrameSlot` stores the complete packet from RenderLoop publication through
RHILoop execution. RHILoop clears the packet only after
`RenderDevice::Execute()` has consumed the command list and the frame is marked
done. Releasing the frame scope decrements pin counts; if a released transient
resource has no more pins, Context may reset the `UPtr<RHI::T>` payload.

If a slot starts a new generation while an older ready payload is still pinned,
the old payload is moved into the slot's retired-payload storage instead of
being destroyed. That storage is still owned by `ResourceSlot<T>` and is cleared
after the last live pin drops. This preserves raw observer validity without
making command lists or pins own the payload.

## Part 6 — GPU Lifetime After Execute

Frame pins and usage tokens are deliberately orthogonal.

Pins cover this window:

```text
Renderer::Render() -> RHILoop RenderDevice::Execute()
```

They keep CPU-side `RHI::T` objects alive while command-list observer pointers
are read by `Execute()`.

Usage tokens and `DeletionQueue` cover this later window:

```text
RenderDevice::Execute() submission -> GPU timeline completion
```

During `RenderDevice::Execute()`, the Vulkan backend creates a frame completion
token from the timeline semaphore's next signal value. `RHI::UsageVisitor`
walks every pass, command, material payload, and attachment descriptor, and
calls `UpdateLastUsageToken(FrameToken)` on every referenced `GpuResource`.

When a Resource payload later releases its `UPtr<RHI::T>`, the concrete Vulkan
RHI object is destructed. Its destructor does not immediately destroy the native
Vulkan handle. Instead, it enqueues a callback into `DeletionQueue` with the
resource's `LastUsageToken`. The callback captures the backend-owned shared
native object, such as a `DeviceTexture`, `DeviceBuffer`, or pipeline object.

`DeletionQueue::Tick()` queries the frame timeline's completed value. A queued
callback may run only after its retire token is less than or equal to the
completed value. Running the callback releases the captured backend object,
whose RAII destructor performs the actual `vkDestroy*` or `vmaDestroy*` call.

If a resource was never referenced by a submitted command list, its
`LastUsageToken` is zero and it can be retired immediately once observed by the
queue. At shutdown, RHI waits idle and drains transfer and deletion queues
before destroying per-frame resources and the allocator.

## Part 7 — Lifetime Policies

All resource families use the same handle, slot, pin, and deletion mechanisms.
Lifetime policy controls what happens after logical refs release a resource and
pins drop:

- `CachedAsset`: reusable assets such as sampled textures, buffers, and
  pipelines may remain cached after pin count and logical ref count return to
  zero. Releasing the last `ResourceRef<T>` makes the resource entry
  cache-eligible; eviction is a cache policy decision, not a pin-side effect.
- `Transient`: view-scoped resources such as camera render targets are released
  when their last `ResourceRef<T>` owner replaces or clears the handle, and
  their payload is reset after live pins drop.

Boundary rules:

- `ResourceHandle<T>` must stay passive and must not call back into Manager or
  Context from its destructor.
- `ResourceRef<T>` must not expose payload pointers; render code still acquires
  through `FrameResourceScope`.
- `ResourcePin<T>` must not be used as a cache retention or eviction signal.
- `ResourceSlot<T>` must not be used as a logical ownership counter or cache
  policy object.
- `ResourceContext` remains the only owner of resource entries, slots, and
  ready payloads.
- `Resource::Manager` must not grow per-family loading code; that belongs in
  request partitions.
- Request partitions must not own registry maps, ref counts, or frame pins.

`ResourceManager::Clear()` asks Context to force-release every payload during
shutdown, while still respecting live pins until their destructors run.
`ResourceManager::CollectReleasedResources()` is not the release operation; it
only erases transient entries whose last ref already requested release and whose
live pins have already dropped.

## Part 8 — Shutdown Order

Shutdown must release CPU observers before tearing down backend infrastructure:

1. Stop and join Render/RHI threads.
2. Detach and reset the application and renderer.
3. Clear frame snapshots, command lists, and `FrameResourceScope` pins.
4. Call `ResourceManager::Clear()`.
5. Call `RenderDevice::Destroy()`.

This order ensures command-list observers and Resource pins are gone before
ResourceContext releases payloads, and payload destructors can still enqueue
backend-native retirement before the RHI singleton and Vulkan allocator are
destroyed.

## Consequences

The current design makes ownership explicit:

- handles identify resources but do not keep them alive;
- ResourceContext owns entries, slots, logical ref counts, GPU-pending queues,
  and ready payloads;
- ResourceManager is a facade over Context, not another lifecycle owner;
- frame scopes bridge raw command-list observers from Render to RHI execution;
- RHI usage tokens and Vulkan deletion queues bridge submitted GPU work to
  actual native destruction.

The cost is more visible lifecycle code. Renderers must acquire resources before
emitting commands and must handle not-ready resources deliberately. Resource
requests require generation checks and explicit state publication.

That cost is intentional. It prevents hidden frame stalls, avoids accidental
cross-module ownership, keeps `RHI` independent from `Resource`, and separates
CPU observer safety from GPU completion safety.

## Non-Goals

This ADR does not define a full cached-asset eviction policy.
It does not introduce render-graph resource lifetime inference.
It does not define priority scheduling or cancellation for async resource work.
It does not expose swapchain images as Resource-managed textures.
