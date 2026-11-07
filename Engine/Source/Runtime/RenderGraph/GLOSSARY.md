# RenderGraph

Per-frame render graph: passes declare their resource accesses up front, and a single Compile orders, prunes, and prepares them (ADR 05). A module-level RenderGraph singleton owns the cross-frame facilities (pipeline registry, transient texture pool).

## Language

**Parameter**:
A pass's nested aggregate struct — it IS the pass's resource declaration. View-typed fields and elements of owned dynamic view collections declare accesses; every other field is passthrough. The Parameter and the size and storage of its dynamic view collections are frozen once the pass is added.
_Avoid_: separate declare/record callback pairs, per-pass hand-written field lists

**View (graph)**:
A Parameter field type that is the access declaration in one type: role (RHI usage bits) and direction ride the view's type, so the wiring site initializes each field with the resource identity alone and can never reset the declared role (ADR 05).
_Avoid_: parallel declaration lists, restating role/access at the wiring site

**Identity (RGTexture / RGBuffer)**:
Passive handle into the graph's resource table — the two kinds mirror the RHI resource kinds (ADR 05). Cheap to copy, invalid by default, owns nothing.
_Avoid_: generation counters, a common handle base type

**Usage**:
The role axis of an access declaration (sampled, storage, constant, indirect, attachment, copy), spoken in the RHI usage-bit vocabulary end to end; a view's declared usage must be a subset of the resource's creation usage (ADR 05).
_Avoid_: flattened usage enums that bake resource kind into role values

**Access**:
The Read/Write axis of an access declaration, in Vulkan's sense. A load-from-existing attachment and a read-modify-writing shader both declare Read | Write.
_Avoid_: separate read/write view-type pairs where one direction flag suffices

**Stage class**:
The coarse pipeline class a pass belongs to — Graphics, Compute, RayTracing, Transfer — which together with Usage and Access fixes the state a transition requests, without shader reflection.
_Avoid_: reflection-derived stage inference at Compile

**Prologue transition**:
Destination-only state requirement attached to each surviving pass at Compile — a declaration, not a barrier; the backend's diff tracker materializes a barrier only when tracked state differs (ADR 05). A pass must declare every resource it touches, including device-address pulls and indirect buffers.
_Avoid_: inferring synchronization from binding side effects or reflection

**Last-writer chain**:
The ordering rule: a reader orders after the resource's latest writer; a writer orders after the previous writer and every reader of the previous state. Independent passes keep registration order.
_Avoid_: resource version numbers, full pairwise dependency graphs

**Prune**:
Dropping passes that do not contribute to the frame. A pass is required when it has a side effect (a NeverPrune marker or a write to an imported resource) or when a required pass transitively depends on it; everything else is dropped with its transients never realized. Terminology: prune, never "cull" — culling is the renderer's visibility concern.
_Avoid_: runtime mark-side-effect calls, keeping dead producers alive "just in case"

**Pipeline prune**:
A pipeline pass whose async pipeline is still pending at Compile is dropped with its dependents; if that drops a side-effect pass, the frame degrades softly to an empty pass list rather than erroring.
_Avoid_: renderer-side readiness snapshots as inclusion gates

**Graph-created transient**:
A resource registered on the graph and realized by Compile from the pool. Note the two "transient" vocabularies: RHI-level transient means a per-frame belt slice, while graph-created means graph-pooled — pooled resources are RHI-persistent.

**RGTexturePool**:
Cross-frame reuse pool for transient textures, keyed by the full descriptor. Return is immediate with no GPU-completion gate, safe by single-queue submission ordering and per-frame state initialization (ADR 05).
_Avoid_: pooling inside the RHI backend


**Import**:
Registering an externally-owned RHI resource with the graph. Imported resources are never realized; writes to them are side effects; imported views join the prologue like graph-created ones (ADR 05).
_Avoid_: importing graph-created transients
