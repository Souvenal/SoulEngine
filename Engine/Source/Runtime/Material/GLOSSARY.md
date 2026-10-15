# Material

Renderer-neutral runtime material domain model: material records, their cross-renderer storage ABI, and resolved texture records. It does not own scene serialization, shader compilation, or backend-specific bindings.

## Language

**Material**:
Runtime description of surface appearance — imported CPU values plus resource-backed texture records.

**MaterialRecord**:
Runtime resource record projected from an imported source (YAML or Assimp): loader provenance, common scalar/color properties, PBR extensions, and an ordered flat collection of texture records.

**MaterialHandle / ConstMaterialHandle**:
EnTT resource handles to a MaterialRecord; the const form serves inspection queries while loading, overrides, and render bindings retain the mutable form.

**MaterialSource**:
The loader family that created a MaterialRecord (YAML or Assimp) — independent of the material's display name, which may be duplicated.

**MaterialAlphaMode**:
Alpha coverage policy: Opaque, Mask, or Blend; Mask uses the author's cutoff threshold, and unrecognised imported values map to Unknown.

**TextureType**:
Material-owned texture semantic; importer-specific texture types are normalized to this enum at the import boundary.

**TextureRecord**:
Runtime representation of one material texture slot: its TextureType, source metadata, and the texture ref.

**GpuData (material)**:
Cross-renderer storage-buffer ABI containing resolved PBR factors and logical texture-table indices — never GPU handles or descriptor state.

**Material Instance**:
A named use of a material value with a stable handle identity (scene identity plus instance name, or mesh asset path plus slot index).

**Material Template**:
Future shared definition of shader/pass interfaces and default values that instances override; not represented in V0, where the only supported template is implicit PBR metallic-roughness.
