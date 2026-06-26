# Resource TODO

Future improvement notes. These are not required for the current async
resource v1, but document the next pressure points before this module grows.

## Extension Surface

- Reduce central edits when adding a resource family. Today a new type still
  needs changes in `ResourceTypes.cppm`, `ManagedRHIResourceTypes`,
  `ResourceManager.cppm`, and a request partition.
- Consider a per-family registration pattern where traits, request entry point,
  and public facade metadata live together, while Context and FrameScope derive
  storage from that declaration.
- Keep family-specific loading code out of `ResourceContext` and
  `ResourceManager`; add new `ResourceXxxRequests.cppm` partitions when loading
  logic grows independently.

## Public API

- Add small consumer helpers for common state handling, such as ready/failed
  checks, error fetch, skip/fallback policy, or a typed status result.
- Revisit whether external callers should use only `ResourceRef<T>` plus
  snapshots, or whether any direct handle-returning API is still justified.
- Avoid exposing low-level pin operations. Render code should continue using
  `FrameResourceScope::Acquire()` so raw RHI observer pointers and frame pins
  stay paired.

## Lifetime And Cache Policy

- Define real cache eviction policy for `CachedAsset`: budget accounting, LRU
  or priority, explicit eviction requests, and memory-pressure behavior.
- Clarify shutdown-time `Clear()` behavior for entries that still have live
  frame pins, especially cached entries that cannot be erased immediately.
- Keep documenting the invariant that all `ResourceRef<T>` instances must die
  before `ResourceContext` is destroyed.
- Add hot-reload/recreate policy on top of generation checks instead of treating
  generation as the whole reload story.
- Integrate render-target/transient resources with a future render graph if one
  exists; current transient refs are manual lifetime owners.

## Dependencies

- Add first-class dependency modeling for higher-level resources such as
  materials, meshes, and passes that depend on textures, buffers, and pipelines.
- Define how failed dependencies propagate to consumers: skip, fallback, block,
  or explicit error surface.

## Testing

- Add tests for GPU-pending completion and stale GPU-pending discard paths.
- Add tests for `Clear()` with live pins across cached and transient resources.
- Add tests for `ResourceRef<T>` destruction ordering around shutdown once the
  engine shutdown sequence stabilizes.
