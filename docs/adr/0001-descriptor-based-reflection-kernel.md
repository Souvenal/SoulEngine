# Replace EnTT meta reflection with a descriptor-based reflection kernel

Windows builds on clang 23.1.3 crash with a frontend ICE (SEGV 0xC0000005) whenever a translation unit textually
includes entt, imports a module BMI that carries entt declarations, and instantiates `entt::meta_any` — the
optimizer trips over corrupted IR regardless of optimization level, and no compiler flag avoids it. The toolchain
is pinned, so we removed EnTT meta from the YAML load path entirely and replaced it with a small descriptor-based
kernel in `Core.Util.Reflection`: a closed-variant `FieldValue`, `FieldDescriptor`/`FieldTable` generated
from compile-time member-pointer registration, per-type field tables exported beside their types, and a central
explicit `SceneComponentDescriptor` table for name-driven component construction (emplace-first, rollback on
invalid fields). Editor property panels will consume the same field tables plus a parallel editor-metadata side
table, keeping serialization and presentation registrations separate (the O3DE two-context pattern).

## Considered Options

- **Keep EnTT meta and confine `meta_any` instantiation to safe TUs** (module-internal, no foreign entt-carrying
  BMI imports): rejected because the surviving interfaces would be string/void*-erased, and the editor property
  panel — the next planned meta consumer — lives in a downstream module and would hit the same ICE.
- **RTTR / Boost.Describe / Boost.PFR**: rejected to avoid new dependencies; RTTR shares the static-registry risk
  family, and the codebase's -fno-rtti and clang-modules history make heavy template-erasure libraries a poor bet.
- **Serialize(visitor) functions (cereal/CryEngine style)**: rejected because field existence must be queryable
  without an instance (editor panels, validation) and version-conditional control flow would hide fields.

## Consequences

- All `meta_factory` registrations and the meta-based `AssignYamlValue` are gone; the clang workarounds that
  existed only for them (cross-partition registration placement, registration splitting, named-namespace
  initializer preservation, entt type-hash/equality specializations in `Core.Util.Types`) went with them.
- New reflected types register one field-table accessor next to their definition; new scene components add one
  line to the central descriptor table in `SceneYamlIO.cpp`.
