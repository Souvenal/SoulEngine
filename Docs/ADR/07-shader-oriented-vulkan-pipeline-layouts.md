# ADR 07 — Shader-Oriented Vulkan Pipeline Layouts

## Status

Accepted (2026-07-13)

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

Vulkan graphics pipelines own their shader-oriented descriptor set layouts and
pipeline layout.

ShaderCompiler composes and links the graphics pipeline's selected vertex and
fragment entry points in one compiler request. The resulting
`Shader::GraphicsProgram` contains pipeline-level reflection that records every
resource binding by full reflected parameter path, set, binding, resource type,
and array count. The Vulkan backend lowers that reflection into descriptor set
layouts and a `vk::PipelineLayout` owned by the concrete
`Vulkan::GraphicsPipeline`.

`DescriptorManager` remains a Vulkan backend helper for descriptor pools,
descriptor set allocation/writes, immutable sampler descriptor sets, and the
global bindless texture table. Shared descriptor layout ABI inputs, such as
immutable sampler handles and bindless table limits, are carried by a
`DescriptorLayoutConfig` supplied by `RenderDevice`; pipeline layout generation
does not query `DescriptorManager` for that state. `DescriptorManager` does not
own a global pipeline layout and does not offer a command-buffer `BindTo` API.
Descriptor binding during command recording uses the current
`Vulkan::GraphicsPipeline` layout.

Reflected parameter paths, such as `g_frameView.view`, are shader layout keys.
They may be used to find a binding within one pipeline layout. They are not
Resource cache keys and they do not imply that an RHI resource's debug/cache
name automatically binds it to a shader parameter.

All uniform buffers are lowered as dynamic uniform-buffer descriptors backed by
the backend's constant-buffer upload path. Dynamic offsets are collected in the
order required by the reflected Vulkan set/binding layout.

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

## Non-Goals

This ADR does not introduce a full material system, shader cursor API, render
graph, buffer bindless model, or public RHI exposure of Vulkan set/binding
numbers.
