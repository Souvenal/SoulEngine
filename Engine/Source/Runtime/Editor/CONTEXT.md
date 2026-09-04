# Context: Editor

Editor UI subsystem. Dear ImGui is the engine's only UI system — there is no
UI abstraction layer.

**Namespace:** `SoulEngine`

## Terms

| Term | Definition |
|------|------------|
| **Editor** | Directly owned by `EngineLoop`; owns the ImGui context, `EditorWorld`, WIS platform backend, the UI panel registry, and GPU resources for rendering ImGui draw data through the RHI command list. `Initialize()` establishes the ImGui context and UI panels; `EditorWorld` owns editor-only ECS state; `BindPresentation()` installs the platform backend and requests GPU resources. |
| **EditorWorld** | Editor-owned ECS world containing editor-only entities, systems, dispatcher state, and the viewport camera. It is isolated from the runtime Scene registry. It subscribes to window framebuffer events and translates them to `EditorCameraResizeEvent` events handled by `EditorCameraSystem`. |
| **UIDrawFrame** | Self-owning deep copy of one frame of ImGui draw data. Move-only: `Data.CmdLists` points into its own `Lists` storage. |
| **SnapshotDrawData** | Deep-copies a live `ImDrawData` into a `UIDrawFrame`. Must run on the ImGui thread before the next `NewFrame()`. |
| **UIPanel / UIPanelCallback** | One registered debug/editor panel: a name plus an ImGui immediate-mode callback invoked in registration order during `BuildFrame()`. |
| **EditorCameraComponent** | Editor-only camera component containing camera parameters, editor viewport dimensions, persistent `EditorCameraRenderTargets`, picking readback state, and selected-entity state. |
| **EditorCameraRenderTargets** | Editor-camera-owned persistent bundle containing generic camera G-buffer/SceneColor outputs plus the R8_UNORM selection mask. The bundle is recreated with the editor camera extent and carried by `EditorViewRecord`. |
| **Selected render pixel** | The latest editor scene-view pixel chosen by the user. It is copied into the immutable `EditorSnapshot` and consumed by Renderer post-processing to identify the selected EntityId in the G-buffer. |

## Threading

The ImGui context lives on the engine **main thread**: GLFW callbacks feed
`ImGuiIO` during `Tick()`, and `Editor::BuildFrame(dt)` runs
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
