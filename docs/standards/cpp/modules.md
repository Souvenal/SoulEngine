# C++ Modules

Read this document when creating, splitting, exporting, or moving a C++ module or partition.

## Rules

- An ordinary non-template member may be declared in a module interface and defined once in its attached implementation unit to reduce importer recompilation.
- Use `export module M:Part;` for a module interface partition. Re-export it from the primary interface only when downstream modules need its API.
- Use one colon in a partition name, such as `M:Part`. Express deeper logical nesting with a dot inside the partition name, such as `M:Part.Subpart`.
- Give feature implementation partitions a distinct name such as `module M:Part.impl;`. The interface and implementation partition names are distinct.
- A primary module implementation unit may use `module M;` for declarations from `export module M;`.
- Implementation partitions stay private to the named module. They import their interface partition when required and are not imported or re-exported by the primary interface.
- Runtime features normally use an interface/implementation pair. Pure type, template, concept, and compile-time partitions may remain interface-only.
- A primary interface aggregates public feature interfaces through export imports rather than feature implementation bodies.
- Exported and reachable are different properties. Keep private third-party types in private members when importers do not need to name or instantiate them; introduce PImpl only when a real boundary needs it.
- Shared cross-backend APIs are exported once from the primary interface. Backend partitions supply the platform definition.
- Use ordinary `namespace SoulEngine` blocks and explicit declaration-level `export`; do not use `export namespace`.
- Avoid anonymous and `Detail` helper namespaces. Module linkage keeps non-exported declarations private; non-module tests and tools use ordinary namespace scope.
- Keep module targets as `moduleonly` with C++ modules enabled. Interface `.cppm` sources are public; implementation `.cpp` sources are ordinary target sources. Promote to a static archive only after a reproducible build or link requirement proves it necessary.
