module Renderer.Selection;

import Renderer.Interface;
import Core;

import std;

namespace SoulEngine {

std::unordered_map<String, SPtr<IRenderer>> g_RendererCache       = {};
SPtr<IRenderer>                             g_CurrentRenderer     = nullptr;
String                                      g_CurrentRendererName = {};

[[nodiscard]] auto SelectRenderer(StringView Name) -> std::expected<void, ErrorMessage> {
    if (const auto It = g_RendererCache.find(String(Name)); It != g_RendererCache.end()) {
        g_CurrentRenderer     = It->second;
        g_CurrentRendererName = String(Name);
        return {};
    }

    auto Candidate = RendererFactory::Get().Create(Name);
    if (!Candidate) {
        String     Supported = {};
        const auto Names     = RendererFactory::Get().Keys();
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

[[nodiscard]] auto GetCurrentRenderer() -> SPtr<IRenderer> {
    return g_CurrentRenderer;
}

[[nodiscard]] auto GetCurrentRendererName() -> String {
    return g_CurrentRendererName;
}

auto CloseRenderers() -> void {
    for (auto& [Name, Renderer] : g_RendererCache)
        Renderer->OnDetach();
    g_CurrentRenderer.reset();
    g_CurrentRendererName.clear();
    g_RendererCache.clear();
}

} // namespace SoulEngine
