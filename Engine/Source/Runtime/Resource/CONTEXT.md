# Context: Resource

**Namespace:** `SoulEngine::Resource`

Runtime asset/resource loading context. This context names resource lifecycle states from request through render-consumable availability.

## Terms

| Term | Definition |
|------|------------|
| **Resource** | Runtime object requested by engine systems and eventually consumed by rendering or other runtime workflows. |
| **Resource handle** | Stable typed reference to a resource identity and lifecycle, independent of whether the resource is ready. |
| **Resource generation** | Version of a resource identity used to distinguish current asynchronous work from stale completions. |
| **Resource payload** | Committed runtime object published by a ready resource, such as an RHI `SampledTexture`, buffer, or pipeline. |
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
| **Transfer upload completion** | GPU completion token produced by dedicated transfer-queue upload work. Sampled texture resources use this to delay readiness until uploaded image data is available to shaders. |
| **GPU-pending list** | ResourceManager-owned vector of resources waiting for opaque RHI GPU completion tokens. It is scanned on the RHI thread and rebuilt each tick with entries that are still pending. |
| **RHI commitment** | Resource preparation stage where backend-native RHI objects are created, uploaded, descriptor-published, or otherwise made visible to rendering. |
| **Ready resource** | Resource whose required CPU work, RHI commitment, GPU transfer, and publication steps have completed so a frame may consume it without blocking. |
| **Failed resource** | Resource whose preparation reached a terminal non-fatal error and will not become ready without a new request or reload. |
| **Resource wait policy** | Rule chosen by a resource consumer for what to do when the requested resource is not ready. |
| **Resource dependency** | Resource required by a pass, draw packet, material, or other consumer before coherent work can be emitted. |
| **Skip policy** | Resource wait policy that omits dependent rendering or runtime work for the current frame. |
| **Fallback policy** | Resource wait policy that substitutes a known ready resource when the requested resource is not ready. |
| **Block policy** | Resource wait policy that waits for the requested resource outside normal per-frame runtime execution. |

## Relationships

- A **Resource** has exactly one current **Resource state**.
- **Resource state** is public consumer-facing information, not an internal debug-only phase.
- Consumers may use **Resource state** directly; helper predicates such as ready/failed checks are optional convenience, not required API surface.
- A **Resource handle** refers to a **Resource**, not directly to a **Resource payload**.
- A **Resource key** maps equivalent requests to the same **Resource**.
- A **Resource generation** changes when a resource is reloaded or recreated.
- A **Resource payload** exists only after **RHI commitment** succeeds.
- **RHI commitment** belongs to the RHI thread.
- A **CPU-preparing resource**, **RHI-committing resource**, and **GPU-pending resource** are all non-ready resources.
- A **GPU-pending resource** owns a pending payload plus a **GPU completion token** until the token is complete.
- **GPU completion token** interpretation belongs to RHI; **Resource state** publication belongs to Resource.
- **GPU completion token** is defined by RHI and stored by Resource only while the resource is GPU-pending.
- **Transfer upload completion** is the initial GPU completion source for Async Resource v1 and is produced by the RHI backend's dedicated transfer upload path.
- ResourceManager actively polls **GPU-pending list** entries on the RHI thread. `ResourceHandle::GetState()` remains a state read and does not lazily query RHI completion.
- **GPU-pending list** is rebuilt into a fresh vector on each poll: completed entries publish ready, stale-generation entries are dropped, and still-pending entries move into the next vector.
- Sampled texture resources commonly move from CPU-preparing to RHI-committing to GPU-pending to ready.
- Pipeline resources commonly move from CPU-preparing to RHI-committing to ready, without a GPU-pending upload phase.
- A **Ready resource** is safe for render consumption in the current frame.
- A **Failed resource** is distinct from a resource that is still preparing.
- A **Failed resource** does not automatically trigger engine fatal error; the consuming use-site decides whether to skip, fallback, or escalate.
- Asynchronous completion may publish a **Resource payload** only when its captured **Resource generation** still matches the current resource generation.
- Equivalent **In-flight resource request** instances are coalesced by **Resource key** and return handles to the same **Resource**.
- A **Resource wait policy** is chosen by the consumer of a **Resource**, not by the resource object alone.
- A **Resource dependency** affects the consumer scope that owns it: pass dependencies decide pass execution, draw/material dependencies decide draw execution or fallback.
- **Skip policy** is the default non-blocking behavior; **Fallback policy** is used when a suitable substitute exists; **Block policy** is opt-in for startup, tooling, tests, or other non-frame-path operations.
- **Async Resource v1** includes **Sampled texture resource** and **Pipeline resource**; buffer resources may follow the same model later.
- Render targets and swapchain images are not **Sampled texture resources**. They are RHI/RenderGraph presentation and attachment concepts, not ResourceManager-managed sampled texture assets.
- Constant buffers, per-frame uniform buffers, swapchain images, and transient render targets are outside **Async Resource v1**.
