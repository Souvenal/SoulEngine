export module Renderer;

import Core;
import std;

export import :IRenderer;
export import :RasterRenderer;
export import :RayTracingRenderer;
export import :PostProcess.EditorPostProcess;

namespace SoulEngine {

namespace {

std::unordered_map<String, SPtr<IRenderer>> g_RendererCache       = {};
SPtr<IRenderer>                             g_CurrentRenderer    = nullptr;
String                                      g_CurrentRendererName = {};

} // namespace

} // namespace SoulEngine

export namespace SoulEngine {

/// @brief Create the renderer configured by `[Render].DefaultRenderer`.
///
/// @details When the TOML field is absent, `Raster` is selected. Concrete
/// renderer types self-register with RendererFactory in their own partitions.
/// @return A renderer instance, or an error when the configured name has not
///         been registered.
[[nodiscard]] inline auto CreateDefault() -> std::expected<UPtr<IRenderer>, ErrorMessage> {
    const auto& RenderCfg = ConfigManager::Get().GetConfig().Render;
    const auto  Name      = RenderCfg.DefaultRenderer.value_or("Raster");
    LogInfo("Configured default renderer: '{}'", Name);

    if (!RendererFactory::Get().Contains(Name)) {
        String Supported = {};
        const auto Names = RendererFactory::Get().Keys();
        for (std::size_t Index = 0; Index < Names.size(); ++Index) {
            if (Index > 0)
                Supported += ", ";
            Supported += Names[Index];
        }
        return std::unexpected(ErrorMessage(
            Format("Unsupported default renderer: '{}'. Supported renderers: {}", Name, Supported)));
    }

    return RendererFactory::Get().Create(Name);
}

/// @brief Select an engine-level renderer, creating and attaching it on first use.
[[nodiscard]] auto SelectRenderer(StringView Name) -> std::expected<void, ErrorMessage> {
    if (const auto It = g_RendererCache.find(String(Name)); It != g_RendererCache.end()) {
        g_CurrentRenderer     = It->second;
        g_CurrentRendererName = String(Name);
        return {};
    }

    auto Candidate = RendererFactory::Get().Create(Name);
    if (!Candidate) {
        String Supported = {};
        const auto Names = RendererFactory::Get().Keys();
        for (std::size_t Index = 0; Index < Names.size(); ++Index) {
            if (Index > 0)
                Supported += ", ";
            Supported += Names[Index];
        }
        return std::unexpected(
            ErrorMessage(Format("Unsupported renderer: '{}'. Supported renderers: {}", Name, Supported)));
    }

    if (auto R = Candidate->OnAttach(); !R)
        return std::unexpected(R.error().Append(Format("Renderer '{}' attachment failed", Name)));

    SPtr<IRenderer> Renderer{std::move(Candidate)};
    g_RendererCache.emplace(String(Name), Renderer);
    g_CurrentRenderer     = std::move(Renderer);
    g_CurrentRendererName = String(Name);
    return {};
}

/// @brief Return the renderer selected for newly built frame slots.
[[nodiscard]] auto GetCurrentRenderer() -> SPtr<IRenderer> {
    return g_CurrentRenderer;
}

/// @brief Return the current renderer factory key.
[[nodiscard]] auto GetCurrentRendererName() -> String {
    return g_CurrentRendererName;
}

/// @brief Detach and release all cached renderers.
auto CloseRenderers() -> void {
    for (auto& [Name, Renderer] : g_RendererCache)
        Renderer->OnDetach();
    g_CurrentRenderer.reset();
    g_CurrentRendererName.clear();
    g_RendererCache.clear();
}

} // namespace SoulEngine
