# Context: Resource

**Namespace:** `SoulEngine::Resource`

Runtime asset/resource loading context. This context names resource lifecycle states from request through render-consumable availability.

## Terms

| Term | Definition |
|------|------------|
| **Resource** | Runtime object requested by engine systems and eventually consumed by rendering or other runtime workflows. |
| **Resource handle** | Passive typed ticket for a resource key and generation, independent of whether the resource is ready. |
| **Resource ref** | Move-only logical owner for a resource request. It tracks that a runtime system still wants the resource, but does not expose the ready payload. |
| **Resource entry** | ResourceContext-owned registry node for one canonical key. It owns request coalescing metadata such as logical ref count and lifetime policy, and contains the slot for the current generation. |
| **Resource slot** | Internal payload state machine for one resource entry. It owns generation, state, error, and ready payload, but not logical ownership policy. |
| **Ready resource observer** | Raw pointer returned by `Resource::Manager::TryGetReady(ref)` for immediate command-list recording or inspection. It does not own or extend payload lifetime. |
| **Resource generation** | Version of a resource identity used to distinguish current asynchronous work from stale completions. |
| **Resource payload** | Committed runtime object published by a ready resource, represented as `Resource<T>` with a ResourceContext-owned `UPtr<T>`. |
| **Resource name** | Human-authored runtime identity for a resource request, such as a material texture name or buffer name. It may contribute to a Resource key, but it is not a shader binding name and does not bind the resource to a shader parameter by itself. |
| **Resource key** | Canonical identity used to deduplicate equivalent resource requests. It may be derived from a resource name, normalized path, descriptor contents, or a combination of request fields. |
| **In-flight resource request** | Resource request that has been accepted and has not yet reached ready or failed state. |
| **Sampled texture resource** | Resource-system identity for a sampled texture asset request; its ready payload is an RHI `SampledTexture`. It does not represent render targets or swapchain images. |
| **Resource array** | Mutable non-global `Array<T>` Resource-layer object for one ordered array of managed resources. It owns `ResourceRef<T>` values and produces ready `RHI::ResourceArray<T>` snapshots with non-owning observers. It is not a ResourceManager registry entry. |
| **Sampler resource** | Resource-system identity for a small RHI sampler profile such as linear-repeat or anisotropic-repeat; its ready payload is an RHI `Sampler`. |
| **Buffer resource** | Resource-system identity for a buffer request; its ready payload is an RHI buffer. |
| **Pipeline resource** | Resource-system identity for a graphics pipeline request; its ready payload is an RHI graphics pipeline. |
| **Mesh resource** | Cached high-level asset imported through Assimp. It owns parsed submesh metadata, CPU vertex/index arrays, and passive handles for the requested SOA GPU buffers. |
| **Async Resource v1** | Current asynchronous scope covering sampled textures, vertex/index buffers, graphics pipelines, samplers, render targets, constant buffers, and imported meshes. |
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
| **Resource dependency** | Resource required by a pass, draw instance, material, or other consumer before coherent work can be emitted. |
| **Skip policy** | Resource wait policy that omits dependent rendering or runtime work for the current frame. |
| **Fallback policy** | Resource wait policy that substitutes a known ready resource when the requested resource is not ready. |
| **Block policy** | Resource wait policy that waits for the requested resource outside normal per-frame runtime execution. |
| **Cached asset lifetime** | Lifetime policy for reusable assets such as sampled textures, buffers, and pipelines. Ready payloads may remain cached until `ResourceManager::Clear()`. |
| **Transient lifetime** | Lifetime policy for view/frame-scoped resources such as camera render targets. The last `ResourceRef` release requests immediate payload destruction. |
| **Released transient collection** | Cleanup pass that erases transient entries only after their last logical ref already requested release and the payload has been released. It does not initiate release. |

## Relationships

- A **Resource** has exactly one current **Resource state**.
- **Resource state** is public consumer-facing information, not an internal debug-only phase.
- Consumers may use **Resource state** directly; helper predicates such as ready/failed checks are optional convenience, not required API surface.
- A **Resource handle** refers to a resource identity and generation, not directly to a **Resource payload**.
- A **Resource handle** stores only **Resource key** and `ResourceGeneration`; it does not hold `SPtr<ResourceSlot<T>>`, does not call back into `Resource::Manager`, and does not keep the payload alive.
- A **Resource ref** tracks logical ownership. It is created by `Resource::Manager`, retains a non-owning observer for the creating `ResourceContext`, and calls that context to release logical demand. When the resource entry's ref count reaches zero, the entry's lifetime policy decides whether the resource is merely cache-eligible or should release its payload.
- A **Resource entry** owns registry/cache metadata. New lifetime policy fields, logical ref counts, eviction markers, budget metadata, and reload bookkeeping belong here or in `ResourceContext`, not in the slot.
- A **Resource slot** owns payload readiness state only. New state-machine transitions, generation checks, publish results, and payload reset belong here.
- `ResourceContext` is the sole owner of resource entries, slots, and ready payloads. `Resource::Manager` is the public facade over that context.
- `Resource::Manager` may own the `ResourceContext` instance, but it is not a second lifecycle owner. Manager should stay a facade for public Runtime call sites.
- Supported RHI payload families require both `ResourceTraits<RHI::T>::Info` and inclusion in `ManagedRHIResourceTypes`; unsupported `Resource<T>` / `ResourceHandle<T>` / `ResourceSlot<T>` instantiations fail at compile time.
- Supported asset payload families require both `ResourceTraits<T>::Info` and inclusion in `ManagedAssetResourceTypes`. `ManagedResourceTypes` combines the RHI and asset family lists for `ResourceContext` storage.
- Consumers must query `Resource::Manager::GetState(handle)` and `GetError(handle)` instead of resolving payloads from the handle. Renderers resolve ready observer pointers through `Resource::Manager::TryGetReady(ref)`.
- A successful **Ready resource observer** requires matching generation and `Ready` state.
- A **Ready resource observer** does not keep the ResourceContext-owned payload alive. The owning renderer, scene, or application must keep a `ResourceRef<T>` alive for every resource whose observer pointer is recorded into a command list.
- A **Resource key** maps equivalent requests to the same **Resource**.
- A **Resource name** is caller-facing identity; a **Resource key** is cache identity.
  Equivalent names and request descriptors may normalize to the same key.
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
- Sampler resources derive cache identity from a canonical sampler profile and commonly move from RHI-committing directly to ready.
- A **Ready resource** is safe for render consumption in the current frame.
- A **Failed resource** is distinct from a resource that is still preparing.
- A **Failed resource** does not automatically trigger engine fatal error; the consuming use-site decides whether to skip, fallback, or escalate.
- Asynchronous completion may publish a **Resource payload** only when its captured **Resource generation** still matches the current resource generation.
- Equivalent **In-flight resource request** instances are coalesced by **Resource key** and return handles to the same **Resource**.
- A **Resource wait policy** is chosen by the consumer of a **Resource**, not by the resource object alone.
- A **Resource dependency** affects the consumer scope that owns it: pass dependencies decide pass execution, draw/material dependencies decide draw execution or fallback.
- **Skip policy** is the default non-blocking behavior; **Fallback policy** is used when a suitable substitute exists; **Block policy** is opt-in for startup, tooling, tests, or other non-frame-path operations.
- **Async Resource v1** includes **Sampled texture resource**, **Vertex buffer resource**, **Index buffer resource**, **Pipeline resource**, **Sampler resource**, **Render target resource**, **Constant buffer resource**, and **Mesh resource**. A resource array is a local owner object that composes managed resources rather than an additional async resource family.
- A Mesh publishes `Ready` after Assimp import succeeds and its child buffer requests have been submitted. It does not wait for every child GPU upload to complete; draw expansion emits passive handles and Renderer skips the draw until all required buffers are ready.
- Mesh import currently creates one SOA stream per submesh attribute: position (`float3`), normal (`float3`), tangent (`float4`), UV (`float2`), and a `Uint32` index buffer.
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
| `ResourceRef<T>` | Public owner API | Does a runtime system still want this resource? | Logical demand from a runtime system, plus the creating `ResourceContext` observer needed to release that demand. | Payload pointers, cache eviction details, async publication. |
| `ResourceHandle<T>` | Passive ticket API, exposed through refs, snapshots, and state/readiness queries | Which key and generation is this reference about? | Canonical key plus generation. | Slot ownership, logical ownership, destructor side effects, or Manager callbacks. |
| Resource entry | `ResourceContext` internals | What registry record exists for this key? | Ref count, lifetime policy, cache/eviction metadata, request coalescing for one key. | RHI payload state transitions or command-list observer safety. |
| `ResourceSlot<T>` | Resource internals | What is the payload state for this generation? | Generation, state, error, and ready payload. | Logical ref count, cache policy, budget policy, request coalescing, or command-list observer lifetime. |
| `ResourceContext` | Resource internals | Where does lifecycle state live? | Resource families, entries, slots, ready payloads, ref counts, lifetime policy state, and GPU-pending queues. | Public facade behavior or family-specific loading code. |
| `Resource::Manager` | Public Runtime facade | What API should the rest of Runtime call? | The singleton `ResourceContext` instance and public request/query/tick/clear/collect entry points. | Slot internals, entry maps, per-family loading implementation, or ownership hidden in handles. |
| Request partitions | Resource internals | How does one resource family prepare work? | Key derivation, CPU preparation, RHI-thread creation/upload, and result publication for one family. | Registry ownership, logical ref counts, observer lifetime, or public owner semantics. |

When a new feature needs to answer "does anyone still want this resource?",
modify `ResourceRef`/Resource entry/`ResourceContext`. When it needs to answer
"what state is this generation's payload in?", modify `ResourceSlot`.
Do not use ready observer pointers as cache-retention signals, and do not add
Manager callbacks to `ResourceHandle<T>`.

When code needs entry maps, ref counts, lifetime policy application, or
GPU-pending queues, it belongs in `ResourceContext`. When code needs texture
decode, mesh import, shader compilation, RHI object creation, or upload
submission, it belongs in a request partition. When code only adapts Resource
for external callers, it belongs in `Resource::Manager`.

`CollectReleasedResources()` is a collection pass, not a release trigger. A
transient resource is released by last-ref release through `ResourceRef<T>` and
`ResourceContext::ReleaseRef()`. Collection later erases the entry only after
`ResourceSlot<T>` reports that the payload has already been released.

`ManagedRHIResourceTypes` and `ManagedAssetResourceTypes` are the central
family lists used by `ResourceContext` and request helpers.
`ManagedResourceTypes` combines both. `ResourceTraits<T>` describes how a
listed family behaves. A type with traits but not in its managed list is
deliberately not a `ManagedResource`, because Context would not have storage
for it.

## Extension Guide

This section documents the current Resource module extension surface. It is
also the baseline for future refactors: adding a new resource type should
eventually require fewer central edits than it does today.

### Current file roles

| File | Role |
|------|------|
| `ResourceTypes.cppm` | Public typed resource primitives: state enums, lifetime policy, `ResourceTraitInfo`, `ResourceTraits<T>`, managed RHI/asset family lists, `Resource<T>`, `ResourceSlot<T>`, `ResourceHandle<T>`, and Mesh structures. |
| `ResourceContext.cppm` | Internal lifecycle owner: typed resource families, resource entries, request coalescing, logical ref counts, lifetime policy application, state publication, GPU-pending queues, clear, and released-transient collection. |
| `ResourceManager.cppm` | Public facade over `ResourceContext`; owns the singleton context, defines `ResourceRef<T>`, creates valid refs after Context accepts logical demand, and exposes request/state/query APIs. |
| `ResourceRequestCommon.cppm` | Internal request-flow helpers shared by resource request partitions: begin request work, publish ready/failed/GPU-pending results, mark RHI commit, and produce consistent stale/shutdown logging. |
| `ResourceTexture.cppm` | Sampled-texture submit flow: key normalization, CPU decode, async task scheduling, RHI upload creation, and result publication. |
| `ResourceSampler.cppm` | Sampler submit flow: canonical descriptor key derivation, RHI sampler creation, and ready/failure publication. |
| `ResourcePipeline.cppm` | Graphics-pipeline submit flow: key creation, shader compilation/preparation, RHI pipeline creation, and result publication. |
| `ResourceBuffer.cppm` | Buffer submit flows: vertex/index buffer data validation/copy, RHI buffer creation, GPU-pending upload publication, constant buffer creation, and failure publication. |
| `ResourceRenderTarget.cppm` | Render-target submit flow: transient key request orchestration, RHI creation, and result publication. |
| `ResourceMesh.cppm` | Assimp mesh submit flow: normalized asset path, CPU import, SOA submesh extraction, child vertex/index-buffer requests, and Mesh publication. |
| `Resource.cppm` | Public aggregate module exporting `:Types` and `:Manager`. |

### Current steps to add a resource type

1. Add or expose the payload type in RHI or the owning runtime module.
2. Add a `ResourceTraits<T>` specialization in `ResourceTypes.cppm`.
   Set:
   - `ResourceGpuPendingPolicy::WaitForCompletion` when the ready payload must
     wait for an RHI `GpuCompletionToken`.
   - `ResourceGpuPendingPolicy::None` when RHI creation can publish `Ready`
     directly.
   - `Label` for logs and diagnostics.
   - `DefaultPolicy` for cache/eviction behavior.
3. Add RHI payloads to `ManagedRHIResourceTypes` or high-level assets to
   `ManagedAssetResourceTypes`. `ResourceContext` derives `ResourceFamilies`
   from the combined list. A type is not accepted unless both its list entry
   and `ResourceTraits<T>::Info` exist.
4. Do not add per-type branches for Context lookup, clear,
   released-transient collection, or GPU-pending iteration. These paths derive
   from the managed family lists plus `ResourceTraits`.
5. If the new type uses GPU-pending readiness, update:
   - `ResourceTraits<RHI::T>::Info` to use
     `ResourceGpuPendingPolicy::WaitForCompletion`; `ForEachGpuPendingFamily()`
     derives from traits and the family tuple.
   - The submit flow to call `PublishResourceGpuPending<RHI::T>()` after RHI
     upload submission.
6. If the new type has special release or collection behavior, keep the policy
   decision in the resource entry or `ResourceContext`. Do not add cache or
   owner-count behavior to `ResourceSlot<T>`.
7. Add or extend the relevant `Resource*.cppm` request partition. These
   partitions expose internal `SubmitXxxRequest(...)` functions that return
   handles to `ResourceManager` facade methods; they are not public owner
   APIs. If the new
   resource family has independent workflow, create a new
   `ResourceXxx.cppm` partition and import it from `ResourceManager.cppm`.
   The submit path should:
   - Derive a canonical resource key.
   - Call `BeginResourceWork<T>(Context, Key)`.
   - Start work only when `Work.ShouldStartWork` is true.
   - Use `Work.Graph` to move CPU-only work to background tasks when it is
     non-thread-affine.
   - Use `Work.Graph` to move RHI object creation and upload submission to
     `ThreadQueue::RHI`.
   - Call `MarkResourceRhiCommitting<RHI::T>()` before RHI creation when the
     family owns an RHI payload.
   - Publish terminal failure through `PublishResourceFailed<T>()`.
   - Publish direct readiness through `PublishResourceReady<T>()`, or upload
     readiness through `PublishResourceGpuPending<RHI::T>()`.
   - Keep per-family request partitions focused on key derivation, CPU loading,
     RHI object creation, and any family-specific preparation. Common
     stale/shutdown publish logging belongs in `ResourceRequestCommon.cppm`.
8. Add a public facade method to `ResourceManager.cppm` when external systems
   need to request the new resource type.
9. If render code will pass the resource as a raw RHI observer pointer into a
   command list, make sure the owning scene/application/renderer keeps a
   `ResourceRef<T>` alive, then resolve the pointer through
   `Resource::Manager::TryGetReady(ref)` or `TryGetReady(handle)`.
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

- `ResourceTypes.cppm` for `ResourceTraits<T>` and the appropriate managed
  family list.
- `ResourceManager.cppm` for public request API.
- A `Resource*.cppm` request partition for submit flow.

Future Resource refactors should reduce these central edits. A preferred target
is that a new resource family declares its traits and requests flow in one local
place, while Context/Manager derive behavior from traits or use the centralized
managed-type list. Avoid adding more per-type
`if constexpr` chains unless the change is deliberately temporary and
documented here.
