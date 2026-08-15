# Context: Resource

**Namespace:** SoulEngine

Runtime resource-request and asset-loading layer. Resource owns canonical
request identities, async CPU preparation, and logical demand. RHI owns native
payload creation, submission lifetime, and native destruction.

## Terms

| Term | Definition |
|------|------------|
| **Resource handle** | Passive typed key plus generation. It identifies a request/result but never keeps a payload alive. |
| **Resource ref** | Move-only logical-demand owner returned by ResourceManager. Its last release affects cache/transient entry policy; it is not the final GPU-submission lifetime owner. |
| **Resource entry** | ResourceContext registry node for one canonical key. It owns request coalescing, logical ref count, lifetime policy, and the current generation slot. |
| **Resource slot** | Internal readiness/error state for one generation. It contains the published high-level Resource wrapper, not cache policy. |
| **RHI-backed Resource wrapper** | Ready high-level object such as mesh buffers, texture, pipeline, sampler, render target, BLAS, or TLAS. It owns its native payload through one or more RHIRef<T> values and exposes a copy for command recording. |
| **Ready RHI payload ref** | RHIRef<T> returned by a ready Resource wrapper. A resource-bearing command copies this ref, so command lifetime no longer depends on a raw observer pointer or a live ResourceRef<T>. |
| **RHI dependency waiter** | RHI-thread-polled closure stored by ResourceContext. It observes ref-backed dependency readiness/failure without exposing backend completion-token details. |
| **RHI commitment** | RHI-thread native creation, descriptor publication, or GPU-work submission. Vulkan queues it through ThreadQueue::RHI. |
| **GPU-pending resource** | A resource whose RHI ref payload exists but remains GpuPending until an immediate-context completion callback publishes it ready. |
| **Cached asset lifetime** | Reusable assets may remain in the Resource registry until ResourceManager::Clear(). |
| **Transient lifetime** | View/renderer-scoped resource entries are erased after their last logical ref release and collection. Any previously submitted RHIRef command copy still keeps its payload alive. |

## Ownership split

| Layer | Owns | Does not own |
|-------|------|--------------|
| ResourceRef<T> / ResourceContext | Logical demand, key/generation, cache and transient-entry policy, CPU/import work | GPU-submission lifetime after a command has been recorded |
| Ready Resource wrapper | One or more RHIRef<T> values plus resource-family metadata | Vulkan timeline values or direct native destruction |
| RHIRef<T> | Shared payload state and native payload until last ref release | Resource cache policy or registry identity |
| RHI command list | Copies of every normal persistent resource ref it uses | ResourceManager entries or backend-native context mutation |
| Vulkan in-flight submission | Submitted command list until graphics-timeline completion | High-level Resource request/cache policy |

## Lifecycle

1. A caller requests a Resource and receives a handle/ref pair. Equivalent
   requests coalesce to one Resource entry.
2. CPU preparation runs without native RHI mutation. The resource then queues
   RHI commitment work to ThreadQueue::RHI.
3. Native resource creation returns or publishes an RHIRef<T>. Synchronous
   native objects become Ready; uploads remain GpuPending until an
   immediate-context completion callback runs on the RHI thread.
4. Renderers read Resource readiness, retrieve a ready RHIRef<T> from the
   wrapper, and copy it into commands. `RHIRef::operator bool()` is the Ready
   guard; `operator->`, `operator*`, or `TryGet()` may access the payload for
   recording-time inspection or backend lowering, but the raw pointer is never
   the command lifetime carrier.
5. ResourceManager may collect a released transient entry after RHILoop submits
   the command. Vulkan holds the command list through its graphics timeline,
   so the copied RHIRef prevents premature native destruction.
6. After all higher-level and in-flight refs are gone, RHIRefPayload enqueues
   the native destructor. RHIRenderDevice::Tick() drains it on the RHI thread.

## Threading and shutdown

- CPU file IO, decode, import, shader preparation, request bookkeeping, and
  handle/state observation may run outside RHILoop.
- Resource RHI commitment and TickRhiDependencies() run on RHILoop.
- Resource does not interpret Vulkan timeline values and no longer owns a
  public GPU-completion-token list. Immediate callbacks publish RHI ref
  readiness; Resource waiters observe that published state.
- ResourceManager::CollectReleasedResources() only removes released logical
  entries. It is not proof that a submitted GPU use has retired.
- Engine shutdown clears ResourceManager before RHIRenderDevice::Destroy(),
  ensuring Resource-owned RHIRef values release while the deferred-deletion
  queue and native device are still valid. Escaping refs are unsupported by the
  current raw global deletion queue.

## Dependencies

- Core — resource keys, errors, logging, TaskGraph
- RHI — ref-backed native payloads and RHI-thread creation/readiness
- ShaderCompiler / Material / import libraries — resource-family CPU preparation
- Renderer / Scene / Application — request owners and consumers