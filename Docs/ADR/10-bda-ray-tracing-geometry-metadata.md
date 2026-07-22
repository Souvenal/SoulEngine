# ADR 10: BDA Geometry Metadata for Ray-Tracing Attributes

**Status:** Accepted  
**Date:** July 22, 2026

## Context

Forward rasterization and BLAS construction already consume the Resource-managed
position, normal, and index buffers of each mesh submesh. Ray-tracing
closest-hit attribute lookup must use those same allocations. Binding a distinct
storage descriptor for every mesh stream would expose backend descriptor policy
to Renderer and does not scale across instances or multi-submesh BLAS geometry.

Vulkan ray tracing requires buffer device address (`bufferDeviceAddress`) for
acceleration-structure builds. On devices below Vulkan 1.2, this also requires
`VK_KHR_buffer_device_address` so Vulkan-Hpp can dispatch address queries. The
same capability can represent a source-buffer address in a small shader-visible
geometry metadata table. Slang emits Vulkan physical storage-buffer access for
this path when the ray-tracing target enables `SPV_EXT_physical_storage_buffer`.

## Decision

Use a **RenderDevice-owned `RayTracingGeometryTable`** as the logical source of
ray-tracing geometry metadata.

- Renderer records only `VertexBuffer`/`IndexBuffer` observers and layout intent
  in `UpdateRayTracingGeometryTableCmd`; it never observes a native address,
  descriptor set/binding, or SBT object.
- Vulkan resolves the source buffer device addresses while recording the command,
  writes a fixed metadata storage buffer, and binds that one reflected storage
  buffer by shader parameter path.
- The metadata ABI has a 16-byte header, 16-byte instance records, and 64-byte
  geometry-address records. `InstanceCustomIndex` selects an instance record;
  `GeometryIndex` selects a geometry record relative to that instance; and
  `PrimitiveIndex` selects the three original uint32 indices.
- The closest-hit shader accesses position, normal, and index data through
  physical storage-buffer pointers. It does not use a descriptor array or a
  renderer-local packed geometry allocation.
- The table is device-owned rather than renderer- or pipeline-owned so multiple
  renderers/views and compatible RT pipelines share the backend lifetime
  boundary. A future backend must implement the equivalent below the RHI
  contract or report the capability unavailable.

The Vulkan metadata allocation is host-visible and command recording inserts a
host-write to ray-tracing-shader-read dependency after every table update.
Because that fixed allocation is reused across frames, Vulkan waits for the
last successfully submitted table-consuming graphics timeline token before the
CPU overwrites its metadata; the storage descriptor covers the table's full
fixed capacity, not the previous update's byte range. Source mesh uploads become
Resource-ready only after their transfer completion is observed; BLAS creation
drains pending source uploads before it reads them. This preserves the existing
safe consumption contract across transfer and graphics queue families without
making Renderer synchronize native queues.

## Consequences

- RT uses the same Resource-managed source buffers as Forward and BLAS builds.
- Reflection exposes one fixed `ByteAddressBuffer` metadata binding, not a
  per-buffer descriptor array.
- Source-buffer usage is stamped from the geometry-table command, so deferred
  deletion remains guarded by the frame completion token.
- BDA remains Vulkan-specific below the RHI abstraction. It is not a general
  bindless-descriptor system and does not define material or texture bindless.
- The metadata table has a fixed 4 MiB capacity in this vertical slice. An
  overflow returns an explicit command-recording error instead of silently
  truncating geometry.

## Considered Options

### Renderer-local packed geometry

Rejected. It duplicates Resource-managed mesh data, creates a second upload and
synchronization path, and was the diagnosed source of invalid closest-hit
attributes.

### Descriptor-indexed storage-buffer array

Rejected for this slice. It requires a general descriptor-indexing policy and
would couple logical dynamic selection to backend descriptor allocation. BDA
only needs one fixed metadata descriptor.

### Store arbitrary attributes inside BLAS or TLAS

Rejected. Acceleration structures are opaque traversal data and do not replace
source mesh streams.
