export module Renderer.Selection;

import Core;

import Renderer.Interface;

import std;

namespace SoulEngine {

/// @brief Renderer selected at startup for applications that use the shared
///        default.  Supported values: "Raster", "RayTracing" (concrete
///        renderers self-register with RendererFactory in their own partitions).
export CVar<String> CVarRenderDefaultRenderer{
    "Render.DefaultRenderer", "Raster", "Renderer selected by applications that support the shared default"};

/// @brief Create the renderer configured by the `Render.DefaultRenderer` CVar.
///
/// @details Concrete renderer types self-register with RendererFactory in
///          their own partitions.
/// @return A renderer instance, or an error when the configured name has not
///         been registered.
export [[nodiscard]] inline auto CreateDefault() -> std::expected<UPtr<IRenderer>, ErrorMessage> {
    const auto Name = CVarRenderDefaultRenderer.Get();
    LogInfo("Configured default renderer: '{}'", Name);

    if (!RendererFactory::Get().Contains(Name)) {
        String     Supported = {};
        const auto Names     = RendererFactory::Get().Keys();
        for (std::size_t Index = 0; Index < Names.size(); ++Index) {
            if (Index > 0)
                Supported += ", ";
            Supported += Names[Index];
        }
        return std::unexpected(
            ErrorMessage(Format("Unsupported default renderer: '{}'. Supported renderers: {}", Name, Supported)));
    }

    return RendererFactory::Get().Create(Name);
}

/// @brief Select an engine-level renderer, creating and attaching it on first use.
export [[nodiscard]] auto SelectRenderer(StringView Name) -> std::expected<void, ErrorMessage>;

/// @brief Return the renderer selected for newly built frame slots.
export [[nodiscard]] auto GetCurrentRenderer() -> SPtr<IRenderer>;

/// @brief Return the current renderer factory key.
export [[nodiscard]] auto GetCurrentRendererName() -> String;

/// @brief Detach and release all cached renderers.
export auto CloseRenderers() -> void;

} // namespace SoulEngine
