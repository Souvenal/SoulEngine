# Vulkan

The Vulkan backend — the concrete implementation of the RHI context's contracts (ref lifetime, frame-affined tasks, declaration-driven synchronization).

## Language

### Synchronization

**Graphics timeline**:
Device-level timeline semaphore signalled by every frame submission; frame-completion tokens store its values. Timeline semaphores replace fences in every role (see the Vulkan coding spec).

**Image tracker**:
Device-level per-subresource state tracker keyed by (image, aspect, mip, layer), driving image transitions for the barrier diff tracker. It stores no image metadata — call sites resolve concrete ranges from the payload they already hold — so creation never touches it and it stays RHI-thread confined.

### Memory

**Ring buffer**:
Wrap-around bump-allocation ring over VMA buffers in one of two memory classes: HostMapped (host-visible, contractually host-coherent, persistently mapped) or DeviceLocal (GPU-only working memory). Retirement is frame-granular through frame-begin timeline markers; a failed fit triggers generational growth — a larger replacement buffer takes new allocations and the superseded instance retires through the marker chain. Grow-only, never shrinks.

**Transient belt**:
Typed per-frame allocation lanes (uniform / shader-storage) over a ring buffer. A slice caches its full ring allocation so mid-frame growth with live slices is unambiguous, and binds through dynamic descriptors with the absolute offset. Slices stamp their creation generation: a cross-frame upload — a transient ref retained past its creation frame — is dropped with a warning rather than corrupting a recycled range. RHI-thread confined.

**Staging belt**:
Device-owned frame-start upload staging lane: the staging ring plus pending buffer and image lanes. Upload tasks enqueue payload-blind entries, so batch recording never resolves payloads; the frame-start batch records one aggregated transfer barrier ahead of the acceleration-structure batch, is a no-op when empty, and fails soft per entry (logged, re-issuable). RHI-thread confined.

### Lifetime

**Deferred deletion queue**:
Two hops with distinct jobs (ADR 03). The module-level queue behind the global pointer is the cross-thread transport (see the RHI glossary). The backend queue below is the GPU-retirement hop.

**Backend deferred deletion queue**:
The GPU-retirement hop for native buffers and images and self-owned internal storage (acceleration-structure storage, the ray-tracing shader binding table). Destruction is a virtual timeline event: the enqueue stamp uses a fresh host value no submission signals directly, covered once the next real submission completes — sound because no new command can reference a resource after its last ref is gone, so the destruction frame's own submission upper-bounds the last GPU use. Entries retire from the front in the device tick; shutdown flushes unconditionally while idle. This replaces frame-slot retention for resources referenced outside the command list (upload destinations, device-address geometry buffers).
