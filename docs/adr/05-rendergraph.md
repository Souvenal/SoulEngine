# ADR 05 — RenderGraph: Resource Model, Declaration-Driven Sync, Explicit Present

## Status

Accepted (2026-09-15). Revised 2026-09-24 (declaration-driven synchronization), 2026-09-27 (unified resource and view model), 2026-09-28 (view declarations; usage declared at creation), 2026-10-05 (RHI views carry Access; per-kind access lists replace direction-split vectors), 2026-10-11 (transitions are view + stage — layout derives from the view in the backend; imported resources join the prologue). This is the single ADR for the RenderGraph topic: decisions that were briefly split into ADR 12/13/14 are consolidated here, and future evolution of this design revises this document instead of opening new numbers. Partially supersedes [ADR 06](06-scene-snapshot-and-view-owned-rt.md) (view-owned render-target ownership; the Scene/SceneSnapshot layering stands). Mechanics and type-level details live in the code; this document keeps design and decisions only.

## Context

View-scoped render targets were created with hard-coded usage unions guessed by the wrong layer; transients were realized per Compile with no cross-frame reuse; presentation was an implicit backend blit with no viewport control. Later, use-driven barrier tracking (emitting barriers lazily at binding time from reflection side effects) demonstrably missed declared hazards — SYNC-HAZARD-READ-AFTER-WRITE between culling and the indirect-draw consumers — and cannot in principle cover access channels that never appear as tracked bindings (BDA pulls, indirect-argument ranges). The graph already collected per-pass access declarations for ordering, then discarded them.

The resource layer beneath the graph had its own fragmentation: usage, lifetime, and memory domain were baked into class names (a type zoo), the usage axis existed in three vocabularies with translation tables between them, and a view concept existed without a name — smuggled through binding flags and sentinel values.

## Decision

### Frame model and transients (2026-09-15)

1. **Builder/singleton split.** The per-frame graph object is `RenderGraphBuilder` (UE `FRDGBuilder` analogue); a module-level `RenderGraph` singleton owns cross-frame facilities (PipelineRegistry, `RGRenderTargetPool`, Tick/Shutdown).

2. **Renderer-driven transient targets, pooled.** Renderers create view targets on the builder each frame; graph-created transients come from a cross-frame pool keyed by the full desc. Pool return is immediate (no GPU gate), safe by single-queue submission ordering and per-frame state initialization. The transient/persistent split criterion is whether content carries information across frames; persistent state is Imported.

3. **Explicit present.** Presentation is a graph-visible blit command issued by a `NeverPrune` pass; the swapchain image stays backend-private.

### Synchronization (2026-09-24)

4. **Declaration-driven sync.** Every access a pass makes to a graph resource is declared as a view in its Parameter — *touch it ⇒ declare it*. The graph is the single source of truth for inter-pass synchronization; undeclared access is a pass-author bug surfaced by validation.

5. **Prologue transitions, destination-only.** At Compile, each surviving pass's declarations map through a table to a prologue: the set of states the pass needs its resources in. A transition names only the destination — a view plus a stage mask; it is a state requirement, not a barrier. Fine-grained schema (bit-mask stage/access), deliberately coarse initial mapping (shader accesses → AllGraphics) — precision upgrades are table-data changes. (As written in 2026-09-24 the destination carried an explicit {access, layout} state; the 2026-10-11 revision reduced it to view + stage — see decision 13.)

6. **Emission-side diff tracker.** The backend keeps a single-writer state cache: a prologue entry whose desired state differs materializes exactly one barrier. Tracker update sources are restricted to prologue transitions and backend-reserved sites (swapchain, uploads, host writes, AS builds); all binding/command side-effect tracking is deleted. All passes record into one shared frame secondary so producer writes, prologue barriers, and consumer reads share one command-buffer context (sync validation does not resolve cross-secondary hazards).

### Resource and view model (2026-09-27)

7. **Two resource kinds plus acceleration structures.** RHI exposes `RHIBuffer`, `RHITexture`, and AS — usage, lifetime, and memory domain are no longer class names. Lifetime is chosen at the creation API (persistent / transient-arena / readback); usage unions live on the creation descriptor, mirroring `VkImageUsageFlags`/`VkBufferUsageFlags`.

8. **Value-semantic views.** A view is plain data — identity, role (usage bits), extent (subresource range), direction (Read/Write) — not an RHIObject. One view, one role. Stage is a pass property, never a view property. Bindings and attachments hold views; the backend resolves and caches native views. (As written in 2026-09-27 this excluded direction from views; the 2026-09-28 revision moved `RHIAccess` to the RHI layer precisely so views carry it from declaration through execution — see decision 11.)

### Declaration currency (2026-09-28)

9. **Views are the graph's declaration currency.** RG identities (`RGTexture`/`RGBuffer`, mirroring the RHI kinds) plus RG views create the declaration: the pass declares role/extent/access as field defaults (it alone knows how it consumes), the wiring assigns identities only, and the graph materializes the ready RHI view at Compile for execution to consume verbatim. Role is written exactly once, in the pass, in the same usage vocabulary the graph plans sync with and the backend binds with.

10. **Usage is declared at creation; views are checked against it.** Creation takes the RHI descriptors including a required usage union — what the resource may be used for is the creator's explicit call. Compile checks every surviving view's role against it and fails the frame on under-declaration. This **supersedes decision 3-as-originally-written** (usage derived from surviving views, 2026-09-15): two writings reconciled by *checking*, never by *deriving*.

11. **One Read/Write axis, RHI-level.** Access is `RHIAccess{Read, Write}` — Vulkan's sense — shared by graph declarations and sync states. Role is Usage's axis; transitions carry Usage so the backend maps (Usage, Access) to native access bits at emission. Flattened (role × read-write) product enums are rejected at every layer.

### Frame-start buffer upload batch (2026-09-29, revised same day)

12. **Persistent-buffer creation is synchronous; uploads are decoupled and frame-start batched.** `CreateBuffer` is a pure allocation on the calling thread (thread-safe: VMA locks internally, `vkCreateBuffer` is device-level) and the ref is Ready at return — device addresses are bakable in the requesting frame's geometry table, and the Ready gate needs no special-casing. Buffer Descs never carry data: uploads go through `UploadBuffer(ref, bytes)` (any producer thread), which snapshots the bytes and enqueues a frame-affined task tagged with the producer's frame ordinal. The RHI thread drains the frame's tasks before Execute — staging allocation and host copy happen there — and the frame-start batch records one copy per entry plus one aggregated transfer-write → consumer-read barrier ahead of the BLAS batch, which reads geometry buffers. Frame affinity makes one producer frame's uploads land atomically; the atomic unit is the producer's frame, not the buffer. RenderResult carries no uploads — the packet stays reserved for work the renderer decides per frame (AS builds). Uploaded textures later joined the same transport (revised 2026-10-15): texture creation is synchronous and Ready at return, and texel bytes ride the frame-affined task into the frame-start batch's image lane, safe by submission order — the same contract as GpuOnly buffer uploads. The earlier immediate-context texture path, whose readiness gated bindless descriptor resolution, is superseded.

    (First cut the same day collected refs into RenderResult's pending list and snapshotted into the payload; superseded — the frame-affined task queue already provides the same frame-atomic transport without renderer plumbing, and synchronous creation removes the RHI-thread allocation round-trip entirely.)

### Sync vocabulary reduction (2026-10-11)

13. **Transitions are view + stage; layout is derived, never declared.** Within the supported feature set every layout a pass can need is a pure function of the view's (Usage, Access) — a declared layout validated against that function would carry zero information, so the declaration axis was rejected together with the RHI-level layout enum and the per-kind state structs. The backend's native mapping owns the (Usage, Access) → {access mask, layout} table and translates the declared direction faithfully. Presentation has no layout vocabulary at any layer above the backend: the swapchain's move to the present layout is a backend-internal operation, and the present path stays the explicit blit of decision 3. A future need for a non-canonical layout (e.g. a General-stability policy) arrives as a view hint, not as a revival of the state struct.

14. **Prologue merge accumulates unions.** Several views of one resource (and, for textures, one subresource range) inside one pass merge into a single transition that accumulates the usage, access, and stage union by value; a mixed union (e.g. Sampled | Storage on one range) maps to the shared layout (General) in the backend table. This replaces the previous layout-conflict error: the union's shared layout is the correct answer, not a pass-author violation.

15. **Imported resources join the prologue.** Imported views produce transitions exactly like graph-created ones, and the usage check answers from the realized RHI object's creation usage instead of skipping the check. The backend tracker needs no change: it is device-level and persistent for images, and its correctness rests on the discipline that every resource state change — prologue or backend-reserved site — flows through it.

### Host-visible memory, readback, and AS lifetime (2026-10-15)

16. **All host-visible memory is contractually host-coherent.** Host writes are implicitly visible to later submissions, so no HOST-stage barrier or manual flush exists anywhere in the engine (the D3D12 UPLOAD-heap and Granite linear-allocator precedent). There is deliberately no non-coherent path: the "no barrier" claims of decision 12 and the transient belts' direct binding both rest on this invariant.

17. **Readback destinations are graph-invisible by design.** The readback destination ring is a backend-internal sync site, never a graph resource; the pass declares only the source's transfer-read and carries `NeverPrune`, since the graph-invisible write would otherwise let Compile prune the pass.

18. **TLAS is rebuilt per frame.** The renderer creates a fresh TLAS each frame sized to the instance count; there is no persistent TLAS backing and no refit/update path. Dynamic scenes repack instances every frame anyway, so persistence buys little and costs the whole update and capacity-management machinery.

## Considered alternatives

- **Camera-owned targets with renderer-declared specs** — rejected: simulates usage decisions by hand without graph knowledge.
- **Usage union guessed at creation by a central loader** — rejected: couples one layer to every renderer's needs and over-allocates. (The 2026-09-28 revision moved the union to the *creator's own* explicit declaration, with checking — a different thing from guessing by a wrong layer.)
- **Pool inside the RHI backend** — rejected: the backend cannot know when a resource is idle; lifecycle leaks graph concerns into RHI.
- **Use-driven barrier tracking** — rejected: cannot cover every access channel and demonstrably missed declared hazards.
- **Graph-explicit Before/After with a stateless backend** — rejected: non-graph writes converge in the backend, so the graph cannot always name a correct Before; the state machine must live where all writes converge.
- **Canonical frame-end state contract** — rejected: unnecessary once the backend tracker holds cross-frame state.
- **Immediate-context submission per buffer** — rejected for persistent buffers: N meshes cost N staging allocations and N scattered submissions, and the profiler smears upload work across the timeline instead of one labeled frame-start batch. The texture variant of this path survived 2026-09-29 (bindless descriptor resolution needed readiness gating) and was superseded on 2026-10-15 when texture uploads joined the frame-start batch.
- **Persistent TLAS with refit/update** — rejected (2026-10-15): instances repack every frame in a dynamic scene, so a persistent TLAS adds capacity management, compaction, and update-flag machinery without saving the rebuild it exists to avoid.
- **Non-coherent host-visible memory with explicit flush** — rejected (2026-10-15): one memory discipline everywhere (HOST_COHERENT) removes an entire barrier and flush class from the engine; the platform variance it would accommodate is not one we target.
- **Per-buffer upload lock, latest-data-only** — rejected: cross-buffer skew within one producer frame (some buffers land frame N's data, others N-1's).
- **Per-buffer upload queue with atomic counters** — rejected: preserves per-buffer order but not per-frame atomicity; pushes of two buffers for one frame can straddle the consumer's drain. The atomic unit is the producer's frame, which the frame-affined task queue already models.
- **RenderResult-carried upload list** — superseded the same day it was added: duplicates the frame-affined task transport with extra renderer plumbing, and uploads are not renderer-decided per-frame work like AS builds are.
- **Queued native creation (creation task on ThreadQueue::RHI)** — rejected for persistent buffers: creation is thread-safe (VMA internal locks, device-level vkCreateBuffer), and synchronous creation makes Ready immediate so the requesting frame's geometry table bakes real device addresses.
- **Fully deferred buffer creation (allocation at frame start)** — rejected: creation bakes device addresses on the caller thread, so the allocation must complete before the ref goes Ready.
- **Flattened usage/access enums** — rejected: they bake one axis into another's values; the type zoo and the product enums were the visible warts.
- **Passes constructing execution views standalone** — rejected: transients have no identity before Compile (pooling and pruning pay off exactly there), and views the graph never sees cannot feed declaration-driven sync.
- **Imported backbuffer as the present side effect (UE style)** — rejected: ours is backend-private; `NeverPrune` is the correct equivalent.
- **Declared image layout on views** — rejected (2026-10-11): every layout in the supported feature set is a pure function of the view's (Usage, Access), so a declared layout validated against that function carries zero information; the derivation lives in the backend's native mapping (decision 13).
- **RHI-level present layout** — rejected (2026-10-11): presentation is the swapchain's private concern; the backend performs the move to the present layout natively and no graph node ever names it.
- **Storage-write access widening** — rejected (2026-10-11; it had survived the vocabulary reduction as a backend-mapping rule): the declared direction translates faithfully, matching the attachment loadOp rule — a read-modify-writing shader declares `Read | Write`. Depth/stencil attachment coverage stays always-Read|Write as mechanism-inherent load/store semantics, not a guess about shader behavior.
- **Reflection-derived stage precision at Compile** — rejected: the view-to-parameter association only exists inside Record(), and precision must not depend on pipeline readiness.

## Consequences

- The graph is the single truth for what each pass touches; the mapping table is the single place where sync precision lives; the backend needs no sync intelligence beyond native-barrier mapping and the two-source tracker discipline.
- One vocabulary end to end: RHI usage bits for roles (creation, views, sync planning), `RHIAccess` for direction, views carrying both from declaration through execution.
- The type zoo, tag-encoded declarations, parallel desc types, flattened product enums, and the binding bridge are gone; misuse modes (whole-field wiring assignment, usage-less creation, under-declared creation usage) fail loudly at registration or Compile.
- Deferred: per-view shader-stage hints, intra-pass self-synchronization, graph-side texture upload paths.
- A second backend must implement the blit-to-swapchain command, the diff tracker under the two-source discipline, and view resolution — nothing more.

