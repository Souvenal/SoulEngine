# Context: Editor

Editor UI subsystem. Dear ImGui is the engine's only UI system — there is no
UI abstraction layer.

**Namespace:** `SoulEngine`

## Terms

| Term | Definition |
|------|------------|
| **Editor** | Directly owned by `EngineLoop`; owns the ImGui context, WIS platform backend, the UI panel registry, and GPU resources for rendering ImGui draw data through the RHI command list. `Create()` establishes the ImGui context; `BindWindowSystem()` installs the platform backend and requests GPU resources. |
| **UIDrawFrame** | Self-owning deep copy of one frame of ImGui draw data. Move-only: `Data.CmdLists` points into its own `Lists` storage. |
| **SnapshotDrawData** | Deep-copies a live `ImDrawData` into a `UIDrawFrame`. Must run on the ImGui thread before the next `NewFrame()`. |
| **UIPanel / UIPanelCallback** | One registered debug/editor panel: a name plus an ImGui immediate-mode callback invoked in registration order during `BuildFrame()`. |
| **Selected render pixel** | The latest editor scene-view pixel chosen by the user. It is copied into the immutable SceneSnapshot and consumed by Renderer post-processing to identify the selected EntityId in the G-buffer. |

## Threading

The ImGui context lives on the engine **main thread**: GLFW callbacks feed
`ImGuiIO` during `PollEvents()`, and `Editor::BuildFrame(dt)` runs
`NewFrame` -> panel callbacks -> `Render` each game tick, publishing a
`UIDrawFrame` snapshot through a latest-wins mailbox. Editor selection state, including the selected render pixel, is copied into the SceneSnapshot on the game thread. The **render thread**
is a pure consumer: `OnRender()` takes the newest snapshot and translates it
into an RHI pass (`TranslateImDrawData`). Backend-native GPU resources (font
atlas texture, dynamic vertex/index buffers) are created by a one-shot
RHI-thread task enqueued after `Editor::BindWindowSystem()`.

## Dependencies

- `Core` — logging, config
- `RHI` — command list, GPU resource creation
- `Resource` — UI pipeline/sampler/constant-buffer requests
- `TaskGraph` — RHI-thread resource creation task
- `WindowSystem` — platform backend selection; GLFW native handle for `imgui_impl_glfw`
- Third-party: Dear ImGui (local checkout target, see root `imgui_dir` option)

## Presentation-overlay lifetime

Editor::AttachPresentationOverlay() writes borrowed ImGui snapshot,
texture-queue, and mutex pointers into RHIImGuiPresentationOverlayCmd. This is
an Execute-time bridge for the Dear ImGui backend, not a general RHI resource
reference: RHILoop clears the slot only after Execute() returns, and Vulkan
does not access this borrowed overlay payload after submission. Ordinary engine
GPU resources must use RHIRef<T> in the command list instead.