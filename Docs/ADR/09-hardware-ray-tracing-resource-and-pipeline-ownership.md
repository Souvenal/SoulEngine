> **August 1, 2026 implementation note:** RHI payload ownership described here
> now uses `RHIRef<T>` under the RHI submission-retention contract. A submitted ray-tracing command
> retains pipeline, TLAS, and BLAS refs through Vulkan timeline retirement.
# ADR 09 — Hardware Ray-Tracing Resource and Pipeline Ownership

## Status

Accepted (2026-07-21)

## Context

SoulEngine's current renderer consumes immutable `SceneSnapshot` values and
resolves Resource-managed GPU payloads only when they are ready. `Resource::Mesh`
is a high-level geometry asset: it owns imported CPU geometry and requests the
vertex/index buffers required by renderers. It must remain usable by renderers
that do not use hardware ray tracing.

Hardware ray tracing introduces two acceleration-structure levels with distinct
lifetimes:

- A bottom-level acceleration structure (BLAS) is built from a mesh's immutable
  geometry buffers and can be shared by every instance of that mesh.
- A top-level acceleration structure (TLAS) is built from the render scene's
  current instances and transforms. It changes independently of mesh asset
  identity.

The engine also needs a native ray-tracing pipeline. Vulkan requires the shader
binding table (SBT) to hold opaque shader-group handles with device-address and
alignment rules. Exposing those details to Renderer would break the existing
RHI abstraction and make another backend unnecessarily difficult.

## Decision

### Mesh and acceleration-structure ownership

`Resource::Mesh` remains a geometry-asset source and does not own an
acceleration structure.

`Resource::BottomLevelAccelerationStructure` is an independent Resource type.
It owns the `RHI::BottomLevelAccelerationStructure` GPU payload and retains the
source vertex/index-buffer `ResourceRef`s required by the payload. Its cache
identity is derived from the mesh identity, geometry revision, build flags, and
geometry policy. For the first implementation, one Mesh produces one
multi-geometry BLAS composed from all of its eligible submeshes.

The BLAS is therefore shared by instances, renderer instances, and views. It is
not a Scene component and is not duplicated for every `RenderableInstance`.

`Resource::TopLevelAccelerationStructure` is also an independent Resource type.
It owns an `RHI::TopLevelAccelerationStructure` payload and is scoped to one
ray-tracing renderer/render-scene owner. A TLAS is persistent across frames;
its Resource identity is renderer-scoped rather than derived from the per-frame
scene contents.

### TLAS update policy

`RayTracingRenderer` creates logical instance descriptions from the immutable
`SceneSnapshot` and records an RHI build-or-update command before tracing.

The native backend follows this policy:

- When the incoming instance count fits the allocated capacity and the build
  policy permits updates, update the TLAS.
- When capacity is insufficient or an update is invalid, reallocate the native
  backing objects and rebuild the TLAS.

Frame-varying scene contents must not be encoded in ResourceManager cache keys
or submitted as a new asynchronous asset request every frame.

### RHI boundaries

RHI defines backend-agnostic GPU types:

```text
Pipeline : GpuResource
├─ GraphicsPipeline
└─ RayTracingPipeline

AccelerationStructure : GpuResource
├─ BottomLevelAccelerationStructure
└─ TopLevelAccelerationStructure
```

The common `Pipeline` base supplies the reflection-derived shader-parameter
layout and usage lifetime shared by graphics and ray-tracing pipelines.

RHI also defines backend-neutral triangle geometry, acceleration-structure
instance, build policy, ray-tracing pipeline, and command descriptors. Renderer
code may reference RHI objects and logical shader parameter paths, but it must
not use `Vk*`, `vk::*`, Vulkan descriptor set/binding indices, device addresses,
shader-group handles, or SBT region structures.

### Ray-tracing pipeline and SBT

`RHI::RayTracingPipeline` is parallel to `RHI::GraphicsPipeline`.
`Vulkan::RayTracingPipeline` owns the native pipeline, descriptor/pipeline
layouts, shader-group handles, SBT storage, and the native raygen/miss/hit/
callable SBT regions. `TraceRaysCmd` supplies only logical pipeline identity
and dispatch dimensions. The backend retrieves the SBT internally.

The first runnable pipeline supports one ray-generation group, one miss group,
one triangle closest-hit group, no callable records, and recursion depth one.
The public contract remains extensible to any-hit, intersection, multiple hit
groups, and callable groups.

### Capability policy

Hardware ray tracing is an explicit Vulkan capability, not an assumed graphics
feature. The Vulkan backend must validate the complete extension, feature, and
property set before it reports the ray-tracing path as available. Failure must
return a specific error chain naming the missing requirement. It must not
silently produce a partial ray-tracing implementation.

The rendering/application fallback choice is made by the implementation phase;
the capability layer only reports availability accurately.

## Consequences

- BLAS lifetime is independent of `Resource::Mesh` while retaining the geometry
  buffers required to trace safely.
- One mesh can be instanced many times in one TLAS without duplicating BLAS
  memory or build work.
- TLAS updates fit the existing three-thread model: Renderer emits immutable
  command data and RHI records native updates/dispatches.
- SBT/device-address details stay private to Vulkan, so future backends have a
  stable RHI contract to implement.
- The Resource layer gains two GPU-resource workflows and explicit dependency
  retention, which requires focused async/shutdown tests.

## Considered Options

### Store BLAS inside `Resource::Mesh`

Rejected. This would couple every mesh asset to hardware ray tracing, prevent
build-policy variants, obscure independent GPU lifetime, and make non-RT mesh
users pay for a renderer-specific representation.

### Create one BLAS per Scene instance

Rejected. BLAS describes object-space geometry; per-instance duplication wastes
memory and build time. Instance transforms belong in TLAS entries.

### Re-request a TLAS Resource from each SceneSnapshot

Rejected. TLAS identity is persistent while instance data is frame-varying.
Putting frame contents in Resource keys defeats caching and conflicts with the
command-list handoff between Render and RHI threads.

### Expose SBT handles and regions to Renderer

Rejected. Those are Vulkan-specific native implementation details and would
leak alignment/device-address policy across the RHI boundary.

## Non-Goals

This ADR does not implement Vulkan extensions, acceleration-structure builds,
ray-tracing pipelines, SBT creation, ResourceManager requests, or
`RayTracingRenderer`. It does not define PBR materials, alpha-tested any-hit
shaders, path-tracing accumulation, denoising, dynamic/skinned BLAS updates,
or a non-Vulkan backend implementation.
