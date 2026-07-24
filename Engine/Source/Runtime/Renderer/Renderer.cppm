export module Renderer;

import Core;

export import :IRenderer;
export import :ForwardRenderer;
export import :RayTracingRenderer;

export namespace SoulEngine {

/// @brief Create the renderer configured by `[Render].DefaultRenderer`.
///
/// @details When the TOML field is absent, `Forward` is selected. Concrete
/// renderer types self-register with RendererFactory in their own partitions.
/// @return A renderer instance, or an error when the configured name has not
///         been registered.
[[nodiscard]] inline auto CreateDefault() -> std::expected<UPtr<IRenderer>, ErrorMessage> {
    const auto& RenderCfg = ConfigManager::Get().GetConfig().Render;
    const auto  Name      = RenderCfg.DefaultRenderer.value_or("Forward");
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

} // namespace SoulEngine
