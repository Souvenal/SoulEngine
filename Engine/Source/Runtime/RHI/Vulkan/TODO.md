# Vulkan TODO

## Validation Layer False Positive: VUID-vkCmdWriteTimestamp-None-00830

- Tracy GPU zones write timestamps into per-pass **secondary** command buffers, while `CollectGpu()` records the query-ring resets into the frame's **primary** command buffer. Validation layers up to and including Vulkan SDK 1.4.357 (VVL tag dated 2026-07-27) do not propagate query state across command buffers within one submission, so once Tracy's 64K-slot query ring wraps they report false-positive `VUID-vkCmdWriteTimestamp-None-00830` "query not reset" errors (MessageID `0xeb0b9b05`). Runtime behavior is correct — the reset executes before the secondary writes in the same submission — and Tracy's GPU timings stay valid. Verified 2026-09-21: a probe zone recorded on the primary CB never triggers the report; all reports come from secondary CBs.
- Upstream fix: KhronosGroup/Vulkan-ValidationLayers PR #13116 ("layers: Fix query queue submission", closes #8233), merged into VVL main on 2026-09-16. No released SDK ships it yet.
- Action: after upgrading to a Vulkan SDK whose VVL tag is dated after 2026-09-16, run Sponza for at least ~30 s with validation enabled, confirm no `0xeb0b9b05` reports appear, then remove this entry and the corresponding note on `LabelScope` in `VKProfiling.cppm`. If filtering is desired in the meantime, key on MessageID `0xeb0b9b05`.

## Reflection-Driven Descriptor Binding

- Decide descriptor set allocation and reuse strategy for draw-level shader bindings. Current code uses per-frame scratch descriptor sets allocated per draw from the active pipeline reflection; it does not yet cache/reuse sets by resource tuple or material identity.
- Replace the fixed-size scratch descriptor pool policy with a configurable or growable allocator. Current pool sizes are conservative constants suitable for the prototype path, not a final frame allocator design.
- Add Vulkan lowering for `ResourceArray<T>` element types beyond sampled textures. The Resource and RHI array containers are generic, but only `ResourceArray<SampledTexture>` is currently accepted by shader parameters and lowered by Vulkan. Runtime-sized arrays remain sampled-texture-only; each additional descriptor type needs its own descriptor-indexing feature validation and update lifetime policy.
- Keep bindless texture selection as small shader data, not per-draw texture rebinding. The prototype pushes a texture index through push constants; once material data grows, migrate to a material/object data buffer and push only an index into that buffer.
- Investigate immutable sampler support without weakening reflection-driven layout ownership. Current layout creation is derived purely from shader reflection and sampler resources are bound as mutable draw-level descriptors; a future immutable-sampler path needs an explicit policy source that still keeps Renderer code independent from Vulkan set/binding numbers.
