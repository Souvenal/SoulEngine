# ADR 11 — Editor Active-Scene System Inspection

## Status

Accepted and implemented (2026-09-06)

## Context

`MaterialManager` is no longer a process-wide owner. Mesh and material caches
belong to the `MeshSystem` registered inside each runtime `Scene`. Editor
panels still need current Scene information while Dear ImGui callbacks are
being drawn, and this pattern must generalise to other Scene systems.

The Editor tick currently draws the main menu before registered panels. The
main menu may synchronously replace the active Application. `Launch` resolves
the active Application after `Editor::Tick()` returns, so a Scene reference
captured before the tick would not remain valid through a menu replacement.

## Decision

- `Editor` stores a non-owning `const Scene*` binding. It never owns a Scene and
  panel code never retains the pointer or query results across frames.
- `Editor::Tick()` keeps one phase and draws its member `DrawMainMenu()` first.
  A successful menu-driven Application replacement immediately rebinds the
  Editor pointer to the replacement Application's Scene. Registered panels
  then query the newly bound Scene during the same UI tick.
- `Launch` binds the initial Scene after the initial `OpenApplication()`
  succeeds and unbinds the Editor before `CloseApplication()` during shutdown.
  All Application replacement paths are restricted to the Editor menu path so
  this rebind is authoritative.
- `Scene` exposes a templated read-only system lookup:

  ```cpp
  template <typename T>
  [[nodiscard]] auto GetSystem() const
      -> std::optional<std::reference_wrapper<const T>>;
  ```

  Editor code does not access `SystemScheduler` directly. A missing system
  returns `std::nullopt`.
- `MeshSystem` exposes one live material query:

  ```cpp
  [[nodiscard]] auto CollectMaterialRecords() const
      -> std::vector<ConstMaterialHandle>;
  ```

  Each call enumerates the material handles known by that Scene's mesh system.
  The Materials panel groups them by `MaterialRecord::Source` and
  `SourceAsset`, displays Assets-relative paths and material names, and stores
  only a scalar resource identity for selection. The query does not copy
  `MaterialRecord` values or expose EnTT cache types.

## Consequences

Editor gains a general Scene-system query path without restoring global domain
singletons or coupling panels to a particular system implementation. Queries
observe the latest state at the time of drawing. The non-owning binding imposes
an explicit lifetime invariant: it must be cleared before the bound Scene is
destroyed. Application switching remains immediate, while all switches have a
single Editor-owned rebind point.

This is a read-only inspection boundary. Editing commands, transactions, and
Undo/Redo are outside this decision. Future writable tooling must introduce a
separate command boundary rather than making `GetSystem<T>()` mutable.

## Considered Options

- Restore a global `MaterialManager`: rejected because material cache lifetime
  is Scene-local and would reintroduce cross-Scene ownership.
- Expose `Scene::GetSystems()` to Editor: rejected because it leaks scheduler
  and cache implementation details and permits mutable access through an
  insufficiently const API.
- Split `Editor::Tick()` into menu and panel phases: rejected for now because a
  member Scene binding updated by the first menu step preserves the existing
  single-tick flow.
- Pass a copied material-record array from `Launch`: rejected because the
  Editor should query the owning system at draw time and avoid unnecessary
  deep copies of resource-bearing records.
