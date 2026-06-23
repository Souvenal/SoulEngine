# Context: Resource

**Namespace:** `SoulEngine::Resource`

Runtime asset/resource loading context. This context names resource lifecycle states from request through render-consumable availability.

## Terms

| Term | Definition |
|------|------------|
| **Resource** | Runtime object requested by engine systems and eventually consumed by rendering or other runtime workflows. |
| **Resource handle** | Stable typed reference to a resource identity and lifecycle, independent of whether the resource is ready. |
| **Resource generation** | Version of a resource identity used to distinguish current asynchronous work from stale completions. |
| **Resource payload** | Committed runtime object published by a ready resource, such as an RHI texture, buffer, or pipeline. |
| **Resource key** | Canonical identity used to deduplicate equivalent resource requests. |
| **In-flight resource request** | Resource request that has been accepted and has not yet reached ready or failed state. |
| **Texture resource** | Resource-system identity for a texture request; its ready payload is an RHI texture. |
| **Buffer resource** | Resource-system identity for a buffer request; its ready payload is an RHI buffer. |
| **Pipeline resource** | Resource-system identity for a graphics pipeline request; its ready payload is an RHI graphics pipeline. |
| **Async Resource v1** | Initial async resource scope covering texture resources and graphics pipeline resources. |
| **Resource state** | Lifecycle state describing where a resource is between request, preparation, RHI commitment, readiness, and failure. |
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
- A **Resource handle** refers to a **Resource**, not directly to a **Resource payload**.
- A **Resource key** maps equivalent requests to the same **Resource**.
- A **Resource generation** changes when a resource is reloaded or recreated.
- A **Resource payload** exists only after **RHI commitment** succeeds.
- **RHI commitment** belongs to the RHI thread.
- A **Ready resource** is safe for render consumption in the current frame.
- A **Failed resource** is distinct from a resource that is still preparing.
- A **Failed resource** does not automatically trigger engine fatal error; the consuming use-site decides whether to skip, fallback, or escalate.
- Asynchronous completion may publish a **Resource payload** only when its captured **Resource generation** still matches the current resource generation.
- Equivalent **In-flight resource request** instances are coalesced by **Resource key** and return handles to the same **Resource**.
- A **Resource wait policy** is chosen by the consumer of a **Resource**, not by the resource object alone.
- A **Resource dependency** affects the consumer scope that owns it: pass dependencies decide pass execution, draw/material dependencies decide draw execution or fallback.
- **Skip policy** is the default non-blocking behavior; **Fallback policy** is used when a suitable substitute exists; **Block policy** is opt-in for startup, tooling, tests, or other non-frame-path operations.
- **Async Resource v1** includes **Texture resource** and **Pipeline resource**; buffer resources may follow the same model later.
- Constant buffers, per-frame uniform buffers, swapchain images, and transient render targets are outside **Async Resource v1**.
