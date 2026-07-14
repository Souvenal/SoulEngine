# Vulkan TODO

## Reflection-Driven Descriptor Binding

- Decide descriptor set allocation and reuse strategy for draw-level shader
  bindings. Current code uses per-frame scratch descriptor sets allocated per
  draw from the active pipeline reflection; it does not yet cache/reuse sets by
  resource tuple or material identity.
- Replace the fixed-size scratch descriptor pool policy with a configurable or
  growable allocator. Current pool sizes are conservative constants suitable
  for the prototype path, not a final frame allocator design.
- Add Vulkan lowering for `ResourceArray<T>` element types beyond sampled
  textures. The Resource and RHI array containers are generic, but only
  `ResourceArray<SampledTexture>` is currently accepted by shader parameters
  and lowered by Vulkan. Runtime-sized arrays remain sampled-texture-only;
  each additional descriptor type needs its own descriptor-indexing feature
  validation and update lifetime policy.
- Keep bindless texture selection as small shader data, not per-draw texture
  rebinding. The prototype pushes a texture index through push constants; once
  material data grows, migrate to a material/object data buffer and push only an
  index into that buffer.
- Investigate immutable sampler support without weakening reflection-driven
  layout ownership. Current layout creation is derived purely from shader
  reflection and sampler resources are bound as mutable draw-level descriptors;
  a future immutable-sampler path needs an explicit policy source that still
  keeps Renderer code independent from Vulkan set/binding numbers.
