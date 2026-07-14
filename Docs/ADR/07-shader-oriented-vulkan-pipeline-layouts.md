# ADR 07 — Reflection-Owned Vulkan Pipeline Layouts

## Status

Accepted (2026-07-13)

Amended (2026-07-14): target layout derivation is reflection-only. Resource
names and shader binding names are distinct concepts.

Amended (2026-07-14): draw-level shader resource bindings use shader binding
names and typed RHI resources. Samplers are mutable RHI resources, not immutable
Vulkan layout policy.

## Context

Slang `ParameterBlock<T>` declarations let shader code express resource groups
without manually assigning Vulkan descriptor set and binding numbers. The Slang
compiler produces deterministic reflection for those groups.

The previous Vulkan backend used one global descriptor layout and one shared
pipeline layout owned by `DescriptorManager`. That only works while every
pipeline follows one fixed engine ABI. It also made command recording bind
descriptor sets before the active graphics pipeline was known, which is only
valid when every pipeline layout is identical.

## Decision

Vulkan graphics pipelines own their reflection-derived descriptor set layouts
and pipeline layout.

ShaderCompiler composes and links the graphics pipeline's selected vertex and
fragment entry points in one compiler request. The resulting
`Shader::GraphicsProgram` contains pipeline-level reflection that records every
resource binding by full reflected parameter path, set, binding, resource type,
and array count. The Vulkan backend lowers that reflection into descriptor set
layouts and a `vk::PipelineLayout` owned by the concrete
`Vulkan::GraphicsPipeline`.

`DescriptorManager` remains a Vulkan backend helper for descriptor pools,
descriptor set allocation, and descriptor writes. It must not define fixed set
roles such as "set 0 is the frame UBO" as the source of truth for pipeline
layouts. Descriptor set layouts, descriptor writes, and dynamic offset ordering
are derived from the linked shader reflection for the current pipeline.

Reflected parameter paths, such as `g_frameView.view`, are shader binding names.
They are stable lookup keys within one pipeline layout. They are parameters to
binding/update calls, not properties of Resource objects. They are not Resource
names, Resource cache keys, asset paths, or backend descriptor object names.
Binding connects a shader binding name to a ready RHI resource observer for a
command scope.

Runtime resources still need names. A resource name identifies what the engine
is requesting or caching; a shader binding name identifies where a resource is
bound in one pipeline layout. These names may be equal by convention in a
specific material system, but equality is not implicit binding behavior.

All uniform buffers are lowered as dynamic uniform-buffer descriptors backed by
the backend's constant-buffer upload path. Dynamic offsets are collected in the
order required by the reflected Vulkan set/binding layout.

Draw commands carry typed shader resource bindings keyed by shader binding name.
The first binding API remains explicit per resource family: sampled textures,
samplers, and constant buffers are bound through type-specific command helpers
rather than one untyped variant surface. The backend validates each binding
against the active draw pipeline's reflection and logs a warning for a shader
binding that is declared by reflection but not provided by the command data.

Sampler resources are Resource-managed RHI payloads. Their Resource key is
derived from a canonical sampler profile, not from a shader binding name.
Vulkan lowers sampler bindings as mutable sampler descriptors so command data
can choose the sampler object at draw time. Immutable Vulkan samplers are not
part of the target binding model.

Constant-buffer writes migrate with the same reflection lookup path. A constant
buffer write names the shader binding it targets; the Vulkan backend resolves
that name through the active pipeline reflection and places the dynamic offset
in reflected descriptor order.

## Consequences

The backend can support pipelines whose descriptor layouts differ according to
their shader reflection instead of requiring every shader to match one global
layout object.

Command recording must bind descriptor sets after a graphics pipeline is active
or immediately before draws that depend on descriptor state. The current
pipeline layout is the authority for descriptor set compatibility and dynamic
offset order.

The Resource layer continues to cache `RHI::GraphicsPipeline` objects. A
separate Vulkan pipeline-layout cache is not required initially because each
cached graphics pipeline owns the layout it needs. If many pipelines later share
identical layouts, a backend-internal layout cache may be introduced without
exposing Vulkan details to Resource.

Resource creation APIs should require or derive a resource name for debuggability,
cache identity, and authoring workflows. Descriptor update APIs should require a
shader binding name for binding correctness. Crossing those two domains should be
explicit, e.g. a material entry maps `g_material.albedo` to resource
`Textures/statue.jpg`.

The constant-buffer path uses logical `RHI::ConstantBuffer` objects, Vulkan
dynamic uniform-buffer arena storage, and reflection lookup by shader binding
name. Descriptor writes and dynamic offsets are emitted for the draw's active
pipeline layout.

Runtime array element binding is deliberately deferred. The initial reflection
binding work binds non-array resources by shader binding name and leaves array
element addressing as a Vulkan TODO.

## Non-Goals

This ADR does not introduce a full material system, shader cursor API, render
graph, buffer bindless model, or public RHI exposure of Vulkan set/binding
numbers.
