# Scene TODO

## Hide EnTT implementation details from Scene importers

**Status:** Deferred

`Scene` currently stores `UPtr<entt::registry>` in its exported class layout and
implements its lifetime operations inline in the primary module interface.
With the current MSVC/Xmake named-module behavior, any downstream translation
unit that owns or destroys `Scene` must directly include `<entt/entt.hpp>` so
EnTT's header-only templates are reachable during lifetime instantiation.

Current workaround:

- Keep `Scene` constructor, destructor, move constructor, and move assignment
  `inline`; defining them non-inline in the primary interface currently produces
  MSVC `LNK2005` duplicate-definition errors.
- Add direct EnTT includes and Xmake package dependencies to Scene lifetime
  consumers such as Application, TestApp, Launch, the executable entry point,
  and Scene tests.
- Renderer modules that intentionally traverse the ECS must also directly
  include EnTT and declare a direct package dependency.

Future goal:

- Move the EnTT registry and its destruction path behind a Scene implementation
  boundary (for example an implementation partition or an equivalent hidden
  runtime state).
- Remove the downstream EnTT includes that exist only to support Scene lifetime
  instantiation.
- Remove the primary-interface lifetime `inline` workaround once the definitions
  have one stable implementation location.
- Keep intentional EnTT consumers, such as renderer ECS systems and Scene YAML
  persistence, as explicit direct EnTT dependencies.