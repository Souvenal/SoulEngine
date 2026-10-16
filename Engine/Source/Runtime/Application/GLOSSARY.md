# Application

Application project identity and scene ownership: an Application is one project-level engine unit that owns the mutable Scene.

## Language

**Application**:
Project-level engine unit created through the application factory; owns the mutable Scene and the project identity. It does not own the window, RHI context, renderer, or GPU resources.

**Application Key**:
Factory key identifying one project-level Application and selecting its `Applications/<ApplicationName>/` directory.

**Default Scene Document**:
Mandatory scene document that every Application loads as its initial scene during creation; a missing or structurally invalid document prevents startup, while component warnings are logged and allow startup.

**SceneSnapshot**:
Immutable render-facing copy of the Scene, built by the game thread and published to the frame slot (ADR 06).
