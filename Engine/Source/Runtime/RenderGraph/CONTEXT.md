# Context: RenderGraph

**Namespace:** SoulEngine

Per-frame render graph: a pass's nested `Parameter` struct IS its resource
declaration — view-typed fields derive accesses at `AddPass` via a compile-time
aggregate walk (no reflection, no second list). A single `Compile()` orders,
prunes, realizes live transients, and constructs the surviving passes into a
`RenderPassList`. Cross-frame graphics/compute pipelines live in
PipelineRegistry, not in the per-frame graph object.

## Language

**Parameter**:
A pass's nested aggregate struct. View-typed fields declare how the pass
accesses resources; every other field (samplers, bindless arrays, CPU scalars)
is passthrough and declares nothing. `AddPass<TPass>(Parameter)` moves it into
the graph; the pass instance is constructed from it at Compile.
_Avoid_: separate Declare/record callback pairs, per-pass author-written lambdas

**View**:
A Parameter field type whose type itself is the access declaration. Nine views:
`RGColorRT` (with `.Present` / `.Load` bits), `RGDepthRT`, `RGTextureSRV`,
`RGStorageBufferSRV`, `RGStorageBufferUAV`, `RGIndirectBuffer`,
`RGConstantBufferSRV`, `RGCopySrc`, `RGCopyDst`. Each carries a `.Ref` payload
the graph fills: at AddPass for imported resources, at Compile (after
realization) for graph-created transients. `Record()` reads `m_Parameter.X.Ref`.
_Avoid_: AccessFields-style parallel declaration lists

**Handle**:
Passive index into the graph's resource table (`RGTextureHandle`,
`RGStorageBufferHandle`, `RGConstantBufferHandle`, `RGReadbackHandle`). Cheap to
copy, invalid by default, no ownership.
_Avoid_: generation counters, a common handle base type

**Last-writer chain**:
The ordering rule. A reader orders after the resource's latest writer; a writer
orders after the previous writer and every reader of the previous state.
Independent passes run in registration order. A pass never depends on itself.
_Avoid_: resource version numbers, full pairwise dependency graphs

**Prune**:
Dropping passes that do not contribute to the frame. A pass is required when it
has a **side effect** — `static constexpr bool NeverPrune` on the pass type, a
static `PresentOutput` marker, a `.Present` access, or a write to an imported
resource (visible outside the graph) — or when a required pass transitively
depends on it. Everything else is dead code: never constructed, and its
transients are never realized. Terminology: prune, never "cull" (culling is the
renderer's visibility concern).
_Avoid_: MarkSideEffect runtime calls, keeping dead producers alive "just in case"

**Pipeline prune**:
At Compile, a pipeline pass whose async pipeline is still Pending is dropped
together with every required pass depending on it. Dropping a side-effect pass
means the frame cannot produce its required output: Compile returns an **empty
RenderPassList** (soft degrade, not an error). A Failed pipeline on a required
pass is a hard frame error. An unregistered pipeline pass is a hard error.
Dead passes are pruned before their pipeline is ever queried.
_Avoid_: renderer-side readiness snapshots as inclusion gates

**Single Compile**:
`Compile() -> std::expected<RenderPassList, ErrorMessage>` is the only entry
point: it validates, orders (last-writer chain), prunes (side effects, then
pipelines), realizes live transients via the RHI device, fills view `.Ref`
payloads, and constructs surviving passes. No separate Execute step; a graph
compiles exactly once — rebuild it every frame. `Record()` is invoked later by
the RHI thread, never by Compile.
_Avoid_: two-phase Compile/Execute APIs, Record calls from the graph

**Constructor injection**:
A pass is constructed at Compile from its resolved Parameter: pipeline passes
(`TPass::BuildPipelineRequest()` exists) as `TPass(Parameter, RHIRef<TPipeline>)`
with the registry's Ready pipeline; transfer passes as `TPass(Parameter)`.
Pipeline readiness cannot exist before Compile, so construction cannot happen
at AddPass.
_Avoid_: shell/template pass classes, per-frame pass-author construction code

**Graph-created transient**:
`CreateShaderStorageBuffer` / `CreateConstantBuffer` / `CreateTexture` register
a descriptor only; Compile realizes the RHI object, and only if a surviving
pass touches the resource. InitialData is the resource's initial content — it
says nothing about how passes view the resource. The graph copies InitialData
bytes at registration (callers may hand in spans over temporaries that die
before Compile). Reading a transient nobody wrote is legal: no InitialData
means undefined scratch contents, which is the caller's concern, not the
graph's. InitialData size must equal SizeBytes (mismatch: invalid handle).
_Avoid_: realizing dead transients, caller-lifetime InitialData spans reaching
realize without a graph-owned copy

**Import**:
Registering an externally-owned RHI object (render target, readback buffer)
with the graph. Imported resources are never realized; writes to them are side
effects (visible outside the graph).
_Avoid_: importing graph-created transients, per-frame transient imports as a
test crutch (tests use a factory-registered mock device instead)

**Aggregate walk**:
The compile-time enumeration of a Parameter's fields: brace-acceptance arity
probe (cap 16 fields) + structured bindings, detecting views by their
`RGViewTag` member. A macro generates the per-arity binding boilerplate; it is
never exported from the module.
_Avoid_: reflection libraries, hand-written per-pass field lists

**Testing**:
RenderGraph tests run without a GPU: a `MockRenderDevice` (base
`RHIRenderDevice` defaults + counting transient-create overrides) is registered
in the backend factory as `"Test"` and created through the normal
`RHIRenderDevice::Create` path. RHIRenderDevice methods are non-pure virtuals
with "not implemented" defaults exactly so tests can subclass the base
directly.
_Avoid_: test-only singleton injection setters, pure-virtual RHI interfaces
that force 26-method mocks
