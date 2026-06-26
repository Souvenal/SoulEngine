# Context: Resource

**Namespace:** `SoulEngine::Resource`

Runtime asset/resource loading context. This context names resource lifecycle states from request through render-consumable availability.

## Terms

| Term | Definition |
|------|------------|
| **Resource** | Runtime object requested by engine systems and eventually consumed by rendering or other runtime workflows. |
| **Resource handle** | Passive typed ticket for a resource key and generation, independent of whether the resource is ready. |
| **Resource ref** | Move-only logical owner for a resource request. It tracks that a runtime system still wants the resource, but does not expose or pin the ready payload. |
| **Resource entry** | ResourceContext-owned registry node for one canonical key. It owns request coalescing metadata such as logical ref count and lifetime policy, and contains the slot for the current generation. |
| **Resource slot** | Internal payload state machine for one resource entry. It owns generation, state, error, ready payload, retired pinned payloads, frame pin count, and deferred payload release, but not logical ownership policy. |
| **Resource pin** | RAII typed view held inside `FrameResourceScope`. It is non-owning to callers but prevents the ResourceContext-owned ready payload from being reset while the pin lives. |
| **Frame resource pins** | Per-frame packet of pins held beside a command list until RHILoop finishes `RenderDevice::Execute()`. |
| **Resource generation** | Version of a resource identity used to distinguish current asynchronous work from stale completions. |
| **Resource payload** | Committed runtime object published by a ready resource, represented as `Resource<RHI::T>` with a ResourceContext-owned `UPtr<T>`. |
| **Resource key** | Canonical identity used to deduplicate equivalent resource requests. |
| **In-flight resource request** | Resource request that has been accepted and has not yet reached ready or failed state. |
| **Sampled texture resource** | Resource-system identity for a sampled texture asset request; its ready payload is an RHI `SampledTexture`. It does not represent render targets or swapchain images. |
| **Buffer resource** | Resource-system identity for a buffer request; its ready payload is an RHI buffer. |
| **Pipeline resource** | Resource-system identity for a graphics pipeline request; its ready payload is an RHI graphics pipeline. |
| **Async Resource v1** | Initial async resource scope covering sampled texture resources and graphics pipeline resources. |
| **Resource state** | Exported lifecycle enum returned by a resource handle. Consumers inspect this state directly to understand whether a resource is CPU-preparing, RHI-committing, GPU-pending, ready, failed, stale, or invalid. |
| **CPU-preparing resource** | Resource whose non-thread-affine CPU work is still running, such as file IO, image decode, shader compilation, or request metadata preparation. |
| **RHI-committing resource** | Resource whose RHI-thread work is running, such as backend object creation, descriptor publication, or GPU work submission. |
| **GPU-pending resource** | Resource whose required GPU work has been submitted but whose completion timeline has not yet reached the point required for render-consumable readiness. |
| **GPU completion token** | Public opaque RHI value held by a GPU-pending resource. The Resource layer may ask RHI whether it is complete, but does not define it or interpret backend-specific timeline or fence data. |
| **Transfer upload completion** | GPU completion token produced by dedicated transfer-queue upload work. Sampled texture and vertex/index buffer resources use this to delay readiness until uploaded data is available on the GPU. |
| **GPU-pending list** | ResourceContext-owned vector of resources waiting for opaque RHI GPU completion tokens. It is scanned on the RHI thread and rebuilt each tick with entries that are still pending. |
| **RHI commitment** | Resource preparation stage where backend-native RHI objects are created, uploaded, descriptor-published, or otherwise made visible to rendering. |
| **Ready resource** | Resource whose required CPU work, RHI commitment, GPU transfer, and publication steps have completed so a frame may consume it without blocking. |
| **Failed resource** | Resource whose preparation reached a terminal non-fatal error and will not become ready without a new request or reload. |
| **Resource wait policy** | Rule chosen by a resource consumer for what to do when the requested resource is not ready. |
| **Resource dependency** | Resource required by a pass, draw packet, material, or other consumer before coherent work can be emitted. |
| **Skip policy** | Resource wait policy that omits dependent rendering or runtime work for the current frame. |
| **Fallback policy** | Resource wait policy that substitutes a known ready resource when the requested resource is not ready. |
| **Block policy** | Resource wait policy that waits for the requested resource outside normal per-frame runtime execution. |
| **Cached asset lifetime** | Lifetime policy for reusable assets such as sampled textures, buffers, and pipelines. Unpinned ready payloads may remain cached until `ResourceManager::Clear()`. |
| **Transient lifetime** | Lifetime policy for view/frame-scoped resources such as camera render targets. The last `ResourceRef` release requests payload destruction after live pins drop. |
| **Released transient collection** | Cleanup pass that erases transient entries only after their last logical ref already requested release and their live frame pins have dropped. It does not initiate release. |

## Relationships

- A **Resource** has exactly one current **Resource state**.
- **Resource state** is public consumer-facing information, not an internal debug-only phase.
- Consumers may use **Resource state** directly; helper predicates such as ready/failed checks are optional convenience, not required API surface.
- A **Resource handle** refers to a resource identity and generation, not directly to a **Resource payload**.
- A **Resource handle** stores only **Resource key** and `ResourceGeneration`; it does not hold `SPtr<ResourceSlot<T>>`, does not call back into `Resource::Manager`, and does not keep the payload alive.
- A **Resource ref** tracks logical ownership separately from frame pins. It is created by `Resource::Manager`, retains a non-owning observer for the creating `ResourceContext`, and calls that context to release logical demand. When the resource entry's ref count reaches zero, the entry's lifetime policy decides whether the resource is merely cache-eligible or should release its payload after live pins drop.
- A **Resource entry** owns registry/cache metadata. New lifetime policy fields, logical ref counts, eviction markers, budget metadata, and reload bookkeeping belong here or in `ResourceContext`, not in the slot.
- A **Resource slot** owns payload readiness and frame-safety state only. New state-machine transitions, generation checks, publish results, pin counts, retired pinned payloads, and deferred payload reset belong here.
- `ResourceContext` is the sole owner of resource entries, slots, and ready payloads. `Resource::Manager` is the public facade over that context.
- `Resource::Manager` may own the `ResourceContext` instance, but it is not a second lifecycle owner. Manager should stay a facade for public Runtime call sites.
- Supported RHI payload families require both `ResourceTraits<RHI::T>::Info` and inclusion in `ManagedRHIResourceTypes`; unsupported `Resource<T>` / `ResourceHandle<T>` / `ResourceSlot<T>` instantiations fail at compile time.
- Consumers must query `Resource::Manager::GetState(handle)` and `GetError(handle)` instead of resolving payloads from the handle. Renderers acquire command-list observer pointers only through `FrameResourceScope::Acquire(ref)` or `FrameResourceScope::Acquire(handle)`, both of which immediately store the corresponding pin.
- A successful **Resource pin** requires matching generation and `Ready` state.
- A **Resource pin** exposes observer access to the payload while keeping the ResourceContext-owned payload from being reset.
- Render code that writes RHI observer pointers into a command list must move every corresponding pin into **Frame resource pins**.
- **Frame resource pins** must outlive RHI command execution and may be released only after the frame reaches `RHIDone`.
- A **Resource key** maps equivalent requests to the same **Resource**.
- A **Resource generation** changes when a resource is reloaded or recreated.
- A **Resource payload** exists only after **RHI commitment** succeeds.
- **RHI commitment** belongs to the RHI thread.
- A **CPU-preparing resource**, **RHI-committing resource**, and **GPU-pending resource** are all non-ready resources.
- A **GPU-pending resource** owns a pending payload plus a **GPU completion token** until the token is complete.
- **GPU completion token** interpretation belongs to RHI; **Resource state** publication belongs to Resource.
- **GPU completion token** is defined by RHI and stored by Resource only while the resource is GPU-pending.
- **Transfer upload completion** is the initial GPU completion source for Async Resource v1 and is produced by the RHI backend's dedicated transfer upload path.
- ResourceManager actively polls **GPU-pending list** entries on the RHI thread. `ResourceManager::GetState(handle)` remains a state read and does not lazily query RHI completion.
- **GPU-pending list** is rebuilt into a fresh vector on each poll: completed entries publish ready, stale-generation entries are dropped, and still-pending entries move into the next vector.
- Sampled texture and vertex/index buffer resources commonly move from CPU-preparing to RHI-committing to GPU-pending to ready.
- Pipeline resources commonly move from CPU-preparing to RHI-committing to ready, without a GPU-pending upload phase.
- A **Ready resource** is safe for render consumption in the current frame.
- A **Failed resource** is distinct from a resource that is still preparing.
- A **Failed resource** does not automatically trigger engine fatal error; the consuming use-site decides whether to skip, fallback, or escalate.
- Asynchronous completion may publish a **Resource payload** only when its captured **Resource generation** still matches the current resource generation.
- Equivalent **In-flight resource request** instances are coalesced by **Resource key** and return handles to the same **Resource**.
- A **Resource wait policy** is chosen by the consumer of a **Resource**, not by the resource object alone.
- A **Resource dependency** affects the consumer scope that owns it: pass dependencies decide pass execution, draw/material dependencies decide draw execution or fallback.
- **Skip policy** is the default non-blocking behavior; **Fallback policy** is used when a suitable substitute exists; **Block policy** is opt-in for startup, tooling, tests, or other non-frame-path operations.
- **Async Resource v1** includes **Sampled texture resource**, **Vertex buffer resource**, **Index buffer resource**, and **Pipeline resource**.
- Render targets are Resource-managed attachment resources when requested
  through typed render-target handles. They are not sampled texture resources
  and do not belong to Async Resource v1.
- Camera render targets use **Transient lifetime** through `ResourceRef<RHI::RenderTarget>`.
- Swapchain images remain backend-private presentation targets.
- Constant buffers and per-frame uniform buffers are outside **Async Resource
  v1**.

## Layering Rules

The Resource module intentionally has several internal concepts because they
model different lifetimes and answer different questions. Keep these layers
separate:

| Layer | Visibility | Answers | Owns | Must not own |
|-------|------------|---------|------|--------------|
| `ResourceRef<T>` | Public owner API | Does a runtime system still want this resource? | Logical demand from a runtime system, plus the creating `ResourceContext` observer needed to release that demand. | Payload pointers, frame pins, cache eviction details, async publication. |
| `ResourceHandle<T>` | Passive ticket API, exposed through refs, snapshots, and state/acquire queries | Which key and generation is this reference about? | Canonical key plus generation. | Slot ownership, logical ownership, destructor side effects, or Manager callbacks. |
| Resource entry | `ResourceContext` internals | What registry record exists for this key? | Ref count, lifetime policy, cache/eviction metadata, request coalescing for one key. | RHI payload state transitions or command-list observer safety. |
| `ResourceSlot<T>` | Resource internals | What is the payload state for this generation? | Generation, state, error, ready payload, retired pinned payloads, pin count, deferred payload release. | Logical ref count, cache policy, budget policy, request coalescing. |
| `ResourcePin<T>` | Stored by `FrameResourceScope` | Is this ready payload protected for the frame packet? | Temporary protection for a ready payload while command-list observer pointers may be consumed. | Logical resource ownership or cache retention. |
| `FrameResourceScope` | Public frame-packet helper | Where are pins stored until RHI consumes command-list observers? | Per-frame pins produced by `Acquire(ref)` or `Acquire(handle)`. | Resource requests, cache policy, or async publication. |
| `ResourceContext` | Resource internals | Where does lifecycle state live? | Resource families, entries, slots, ready payloads, ref counts, lifetime policy state, and GPU-pending queues. | Public facade behavior or family-specific loading code. |
| `Resource::Manager` | Public Runtime facade | What API should the rest of Runtime call? | The singleton `ResourceContext` instance and public request/query/tick/clear/collect entry points. | Slot internals, entry maps, per-family loading implementation, or ownership hidden in handles. |
| Request partitions | Resource internals | How does one resource family prepare work? | Key derivation, CPU preparation, RHI-thread creation/upload, and result publication for one family. | Registry ownership, logical ref counts, frame pin storage, or public owner semantics. |

When a new feature needs to answer "does anyone still want this resource?",
modify `ResourceRef`/Resource entry/`ResourceContext`. When it needs to answer
"is the ready payload safe to reset?", modify `ResourceSlot`/`ResourcePin`.
Do not use pins as cache-retention signals, do not use refs as command-list
observer lifetime guards, and do not add Manager callbacks to
`ResourceHandle<T>`.

When code needs entry maps, ref counts, lifetime policy application, or
GPU-pending queues, it belongs in `ResourceContext`. When code needs texture
decode, mesh import, shader compilation, RHI object creation, or upload
submission, it belongs in a request partition. When code only adapts Resource
for external callers, it belongs in `Resource::Manager`.

`CollectReleasedResources()` is a collection pass, not a release trigger. A
transient resource is released by last-ref release through `ResourceRef<T>` and
`ResourceContext::ReleaseRef()`. Collection later erases the entry only after
`ResourceSlot<T>` reports that the payload has already been released and no live
pins remain.

`ManagedRHIResourceTypes` is the central family list used by both
`ResourceContext` and `FrameResourceScope`. `ResourceTraits<T>` describes how a
listed family behaves. A type with traits but not in the list is deliberately
not a `ManagedRHIResource`, because Context and FrameScope would not have
storage for it.

## Extension Guide

This section documents the current Resource module extension surface. It is
also the baseline for future refactors: adding a new resource type should
eventually require fewer central edits than it does today.

### Current file roles

| File | Role |
|------|------|
| `ResourceTypes.cppm` | Public typed resource primitives: state enums, lifetime policy, `ResourceTraitInfo`, `ResourceTraits<T>`, `ManagedRHIResourceTypes`, `Resource<T>`, `ResourceSlot<T>`, `ResourceHandle<T>`, and `ResourcePin<T>`. |
| `ResourceRef.cppm` | Public move-only logical owner type. It depends on `ResourceContext` directly so ref destruction can release logical demand without type-erased callbacks or Manager callbacks. |
| `ResourceContext.cppm` | Internal lifecycle owner: typed resource families, resource entries, request coalescing, logical ref counts, lifetime policy application, state publication, GPU-pending queues, clear, and released-transient collection. |
| `ResourceManager.cppm` | Public facade over `ResourceContext`; owns the singleton context, creates valid `ResourceRef<T>` instances after Context accepts logical demand, and exposes request/state/release APIs. |
| `ResourceRequestCommon.cppm` | Internal request-flow helpers shared by resource request partitions: begin request work, publish ready/failed/GPU-pending results, mark RHI commit, and produce consistent stale/shutdown logging. |
| `ResourceTextureRequests.cppm` | Sampled-texture submit flow: key normalization, CPU decode, async task scheduling, RHI upload creation, and result publication. |
| `ResourcePipelineRequests.cppm` | Graphics-pipeline submit flow: key creation, shader compilation/preparation, RHI pipeline creation, and result publication. |
| `ResourceBufferRequests.cppm` | Vertex/index buffer submit flow: data copy, RHI buffer creation, GPU-pending upload publication, and failure publication. |
| `ResourceRenderTargetRequests.cppm` | Render-target submit flow: transient key request orchestration, RHI creation, and result publication. |
| `ResourceFrameScope.cppm` | Per-frame pin storage for resources whose raw RHI observer pointers are written into command lists. |
| `Resource.cppm` | Public aggregate module exporting `:Types`, `:Ref`, `:Manager`, and `:FrameScope`. |

### Current steps to add a resource type

1. Add or expose the underlying payload type in RHI or the owning runtime
   module. The Resource layer currently manages RHI payloads through
   `Resource<RHI::T>`.
2. Add a `ResourceTraits<RHI::T>` specialization in `ResourceTypes.cppm`.
   Set:
   - `ResourceGpuPendingPolicy::WaitForCompletion` when the ready payload must
     wait for an RHI `GpuCompletionToken`.
   - `ResourceGpuPendingPolicy::None` when RHI creation can publish `Ready`
     directly.
   - `Label` for logs and diagnostics.
   - `DefaultPolicy` for cache/eviction behavior.
3. Add `RHI::T` to `ManagedRHIResourceTypes`. `ResourceContext` derives its
   `ResourceFamilies` tuple from this list, and `ResourceFrameScope` derives
   its pin-storage tuple from the same list. A type is not accepted by
   `ManagedRHIResource` unless both this list entry and
   `ResourceTraits<RHI::T>::Info` exist.
4. Do not add per-type branches for Context lookup, clear,
   released-transient collection, GPU-pending iteration, or frame pin storage.
   These paths derive from
   `ManagedRHIResourceTypes` plus `ResourceTraits`.
5. If the new type uses GPU-pending readiness, update:
   - `ResourceTraits<RHI::T>::Info` to use
     `ResourceGpuPendingPolicy::WaitForCompletion`; `ForEachGpuPendingFamily()`
     derives from traits and the family tuple.
   - The submit flow to call `PublishResourceGpuPending<RHI::T>()` after RHI
     upload submission.
6. If the new type has special release or collection behavior, keep the policy
   decision in the resource entry or `ResourceContext`. Do not add cache or
   owner-count behavior to `ResourceSlot<T>`.
7. Add or extend the relevant `Resource*Requests.cppm` partition. These
   partitions expose internal `SubmitXxxRequest(...)` functions that return
   handles to `ResourceManager` facade methods; they are not public owner
   APIs. If the new
   resource family has independent workflow, create a new
   `ResourceXxxRequests.cppm` partition and import it from `ResourceManager.cppm`.
   The submit path should:
   - Derive a canonical resource key.
   - Call `BeginResourceWork<RHI::T>(Context, Key)`.
   - Start work only when `Work.ShouldStartWork` is true.
   - Use `Work.Graph` to move CPU-only work to background tasks when it is
     non-thread-affine.
   - Use `Work.Graph` to move RHI object creation and upload submission to
     `ThreadQueue::RHI`.
   - Call `MarkResourceRhiCommitting<RHI::T>()` before RHI creation.
   - Publish terminal failure through `PublishResourceFailed<RHI::T>()`.
   - Publish direct readiness through `PublishResourceReady<RHI::T>()`, or
     upload readiness through `PublishResourceGpuPending<RHI::T>()`.
   - Keep per-family request partitions focused on key derivation, CPU loading,
     RHI object creation, and any family-specific preparation. Common
     stale/shutdown publish logging belongs in `ResourceRequestCommon.cppm`.
8. Add a public facade method to `ResourceManager.cppm` when external systems
   need to request the new resource type.
9. If render code will pass the resource as a raw RHI observer pointer into a
   command list, use `FrameResourceScope::Acquire(ref)`. Pin storage is derived
   from `ManagedRHIResourceTypes`; do not add a per-type storage branch.
10. Add focused tests under `Engine/Source/Runtime/Resource/Tests/`.

### When to add a new file

Add a new `ResourceXxxRequests.cppm` partition instead of expanding an unrelated
requests partition when the resource type has any of these properties:

- CPU loading, parsing, import, or compilation logic that is more than simple
  request orchestration.
- Multiple helper structs or helper functions that are meaningful only for one
  resource family.
- Dependencies that should not be pulled into every request provider.
- A workflow likely to grow independently, such as mesh import, material graph
  loading, animation clip loading, or complex texture processing.

Keep shared helpers inside an existing requests partition only when they are
specific to that resource family. Cross-family helpers belong in a separate
small partition only after at least two families actually share the behavior.

### Current incremental-modification pain points

Adding a resource type currently requires central edits in several places:

- `ResourceTypes.cppm` for `ResourceTraits<RHI::T>` and
  `ManagedRHIResourceTypes`.
- `ResourceManager.cppm` for public request API.
- A `Resource*Requests.cppm` partition for submit flow.

Future Resource refactors should reduce these central edits. A preferred target
is that a new resource family declares its traits and requests flow in one local
place, while Context/Manager/FrameScope either derive behavior from traits or
use the centralized managed-type list. Avoid adding more per-type
`if constexpr` chains unless the change is deliberately temporary and
documented here.
