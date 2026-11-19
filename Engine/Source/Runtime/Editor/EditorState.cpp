module;

#include <entt/entt.hpp>
#include <imgui.h>
#include <nlohmann/json.hpp>

module Editor.State;

import Application;
import Core;
import EditorCommands;
import EditorTypes;
import RHI;
import Renderer;
import Scene;
import WindowSystem;
import magic_enum;
import std;

namespace SoulEngine {

constexpr std::size_t kMaxPanelNames     = 64;
constexpr std::size_t kMaxPanelNameBytes = 256;

[[nodiscard]] auto Unavailable(String Reason) -> CommandJson {
    return CommandJson{{"status", "unavailable"}, {"reason", std::move(Reason)}};
}

[[nodiscard]] auto WindowSize(bool IsAvailable, String Reason, Int64 Width, Int64 Height, String Units) -> CommandJson {
    if (!IsAvailable) {
        return CommandJson{{"status", "unavailable"},
                           {"reason", std::move(Reason)},
                           {"width", nullptr},
                           {"height", nullptr},
                           {"units", std::move(Units)}};
    }
    return CommandJson{{"status", "available"},
                       {"reason", nullptr},
                       {"width", Width},
                       {"height", Height},
                       {"units", std::move(Units)}};
}

[[nodiscard]] auto ValidScale(const ImGuiIO* IO) -> bool {
    return IO && std::isfinite(IO->DisplayFramebufferScale.x) && std::isfinite(IO->DisplayFramebufferScale.y) &&
           IO->DisplayFramebufferScale.x > 0.0F && IO->DisplayFramebufferScale.y > 0.0F;
}

[[nodiscard]] auto ValidLogicalSize(const ImGuiIO* IO) -> bool {
    return IO && std::isfinite(IO->DisplaySize.x) && std::isfinite(IO->DisplaySize.y) && IO->DisplaySize.x >= 0.0F &&
           IO->DisplaySize.y >= 0.0F;
}

[[nodiscard]] auto ViewportRect(const CameraViewport& Viewport, const ImGuiIO* IO, bool IsLogical) -> CommandJson {
    if (IsLogical && !ValidScale(IO))
        return Unavailable("framebuffer_scale_unavailable");

    const double ScaleX = IsLogical ? static_cast<double>(IO->DisplayFramebufferScale.x) : 1.0;
    const double ScaleY = IsLogical ? static_cast<double>(IO->DisplayFramebufferScale.y) : 1.0;
    return CommandJson{{"status", "available"},
                       {"reason", nullptr},
                       {"x", static_cast<double>(Viewport.X) / ScaleX},
                       {"y", static_cast<double>(Viewport.Y) / ScaleY},
                       {"width", static_cast<double>(Viewport.Width) / ScaleX},
                       {"height", static_cast<double>(Viewport.Height) / ScaleY},
                       {"units", IsLogical ? "logical_pixels" : "physical_pixels"}};
}

[[nodiscard]] auto TruncateUtf8(const String& Name) -> std::pair<String, bool> {
    if (Name.size() <= kMaxPanelNameBytes)
        return {Name, false};

    std::size_t Offset = 0;
    while (Offset < Name.size() && Offset < kMaxPanelNameBytes) {
        const auto  First  = static_cast<unsigned char>(Name[Offset]);
        std::size_t Length = 0;
        if (First <= 0x7F)
            Length = 1;
        else if (First >= 0xC2 && First <= 0xDF)
            Length = 2;
        else if (First >= 0xE0 && First <= 0xEF)
            Length = 3;
        else if (First >= 0xF0 && First <= 0xF4)
            Length = 4;
        else
            break;
        if (Offset + Length > Name.size() || Offset + Length > kMaxPanelNameBytes)
            break;

        bool IsValid = true;
        for (std::size_t Index = 1; Index < Length; ++Index) {
            const auto Continuation = static_cast<unsigned char>(Name[Offset + Index]);
            IsValid                 = IsValid && (Continuation & 0xC0) == 0x80;
        }
        if (Length == 3) {
            const auto Second = static_cast<unsigned char>(Name[Offset + 1]);
            IsValid           = IsValid && (First != 0xE0 || Second >= 0xA0) && (First != 0xED || Second <= 0x9F);
        } else if (Length == 4) {
            const auto Second = static_cast<unsigned char>(Name[Offset + 1]);
            IsValid           = IsValid && (First != 0xF0 || Second >= 0x90) && (First != 0xF4 || Second <= 0x8F);
        }
        if (!IsValid)
            break;
        Offset += Length;
    }
    return {Name.substr(0, Offset), true};
}

[[nodiscard]] auto BoundedPanels(std::vector<String> Names) -> CommandJson {
    std::ranges::sort(Names);
    Names.erase(std::unique(Names.begin(), Names.end()), Names.end());
    const auto Total = Names.size();

    std::map<String, bool> UniqueItems;
    bool                   IsTruncated = false;
    for (const auto& Name : Names) {
        auto [BoundedName, IsNameTruncated] = TruncateUtf8(Name);
        IsTruncated                         = IsTruncated || IsNameTruncated;
        auto [It, IsInserted]               = UniqueItems.emplace(std::move(BoundedName), IsNameTruncated);
        if (!IsInserted)
            It->second = It->second || IsNameTruncated;
    }

    const auto  UniqueTotal = UniqueItems.size();
    const auto  Returned    = std::min(kMaxPanelNames, UniqueTotal);
    CommandJson Items       = CommandJson::array();
    for (const auto& [Name, IsNameTruncated] : UniqueItems | std::views::take(kMaxPanelNames))
        Items.push_back({{"name", Name}, {"isTruncated", IsNameTruncated}});
    IsTruncated = IsTruncated || UniqueTotal > Returned || Total > UniqueTotal;
    return CommandJson{{"status", "available"},
                       {"reason", nullptr},
                       {"items", std::move(Items)},
                       {"total", Total},
                       {"omitted", UniqueTotal - Returned},
                       {"collisions", Total - UniqueTotal},
                       {"isTruncated", IsTruncated}};
}

[[nodiscard]] auto UnavailablePanels(String Reason) -> CommandJson {
    return CommandJson{{"status", "unavailable"},
                       {"reason", std::move(Reason)},
                       {"items", CommandJson::array()},
                       {"total", 0},
                       {"omitted", 0},
                       {"collisions", 0},
                       {"isTruncated", false}};
}

[[nodiscard]] auto EntityJson(const std::optional<entt::entity>& Entity) -> CommandJson {
    return Entity ? CommandJson(entt::to_integral(*Entity)) : CommandJson(nullptr);
}

[[nodiscard]] auto CollectReadback(const EditorStateSource& Source) -> CommandJson {
    const auto& Data = Source.EditorData;
    if (!Data.PrimaryViewport) {
        return CommandJson{{"status", "unavailable"},
                           {"reason", "viewport_camera_unavailable"},
                           {"isPending", false},
                           {"latestKnown",
                            {{"status", "unavailable"},
                             {"reason", "viewport_camera_unavailable"},
                             {"entity", nullptr},
                             {"sourceFrame", nullptr},
                             {"isValidInScene", nullptr}}}};
    }

    const bool          IsLatestKnown = Data.ReadbackState == EditorReadbackState::LatestKnown;
    std::optional<bool> IsValidInScene;
    if (Source.SceneValue && Data.ReadbackEntity)
        IsValidInScene = Source.SceneValue->GetRegistry().valid(*Data.ReadbackEntity);
    CommandJson LatestKnown{
        {"status", IsLatestKnown ? "available" : "unavailable"},
        {"reason", IsLatestKnown ? CommandJson(nullptr) : CommandJson(Data.ReadbackReason)},
        {"entity", IsLatestKnown ? EntityJson(Data.ReadbackEntity) : CommandJson(nullptr)},
        {"sourceFrame", nullptr},
        {"isValidInScene", IsValidInScene ? CommandJson(*IsValidInScene) : CommandJson(nullptr)},
    };

    String Status = "unavailable";
    String Reason = Data.ReadbackReason.empty() ? "no_completed_readback" : Data.ReadbackReason;
    if (Data.IsReadbackMalformed || Data.ReadbackState == EditorReadbackState::Malformed) {
        Reason = "malformed_payload";
    } else if (Data.IsReadbackPending) {
        Status = "pending";
        Reason = "scheduled_readback_incomplete";
    } else if (IsLatestKnown) {
        Status = "latest_known";
        Reason.clear();
    }
    return CommandJson{{"status", Status},
                       {"reason", Reason.empty() ? CommandJson(nullptr) : CommandJson(Reason)},
                       {"isPending", Data.IsReadbackPending},
                       {"latestKnown", std::move(LatestKnown)}};
}

[[nodiscard]] auto LifecycleName(EditorLifecycle Lifecycle) -> StringView {
    switch (Lifecycle) {
    case EditorLifecycle::Initialized:
        return "initialized";
    case EditorLifecycle::Running:
        return "running";
    case EditorLifecycle::Shutdown:
        return "shutdown";
    case EditorLifecycle::Unknown:
        break;
    }
    return "unknown";
}

auto EditorStateCollector::Collect(const EditorStateSource& Source) const -> CommandJson {
    CommandJson ApplicationState =
        Source.ApplicationValue
            ? CommandJson{{"status", "available"}, {"reason", nullptr}, {"name", Source.ApplicationValue->GetName()}}
            : CommandJson{{"status", "unavailable"}, {"reason", "no_active_application"}, {"name", nullptr}};
    CommandJson RendererState =
        !Source.RendererName.empty()
            ? CommandJson{{"status", "available"}, {"reason", nullptr}, {"name", Source.RendererName}}
            : CommandJson{{"status", "unavailable"}, {"reason", "no_active_renderer"}, {"name", nullptr}};
    RendererState["backend"] =
        Source.RenderBackend ? CommandJson(magic_enum::enum_name(*Source.RenderBackend)) : CommandJson(nullptr);

    const bool  HasPhysicalWindow = Source.Window && Source.Window->GetFramebufferExtent().Width >= 0 &&
                                    Source.Window->GetFramebufferExtent().Height >= 0;
    const auto  PhysicalExtent    = HasPhysicalWindow ? Source.Window->GetFramebufferExtent() : FramebufferExtent{};
    CommandJson Scale =
        ValidScale(Source.IO)
            ? CommandJson{{"status", "available"},
                          {"reason", nullptr},
                          {"x", Source.IO->DisplayFramebufferScale.x},
                          {"y", Source.IO->DisplayFramebufferScale.y},
                          {"units", "physical_pixels_per_logical_pixel"}}
            : CommandJson{{"status", "unavailable"},
                          {"reason", Source.IO ? "framebuffer_scale_unavailable" : "imgui_context_unavailable"},
                          {"x", nullptr},
                          {"y", nullptr},
                          {"units", "physical_pixels_per_logical_pixel"}};
    CommandJson Window{
        {"logical",
         WindowSize(ValidLogicalSize(Source.IO),
                    Source.IO ? "logical_size_unavailable" : "imgui_context_unavailable",
                    ValidLogicalSize(Source.IO) ? static_cast<Int64>(Source.IO->DisplaySize.x) : 0,
                    ValidLogicalSize(Source.IO) ? static_cast<Int64>(Source.IO->DisplaySize.y) : 0,
                    "logical_pixels")},
        {"physical",
         WindowSize(HasPhysicalWindow,
                    "window_context_unavailable",
                    PhysicalExtent.Width,
                    PhysicalExtent.Height,
                    "physical_pixels")},
        {"framebufferScale", std::move(Scale)},
    };

    CommandJson Viewport;
    if (!Source.EditorData.PrimaryViewport) {
        Viewport                           = Unavailable("viewport_camera_unavailable");
        Viewport["physical"]               = nullptr;
        Viewport["logical"]                = nullptr;
        Viewport["isDrawable"]             = false;
        Viewport["hover"]                  = Unavailable("viewport_camera_unavailable");
        Viewport["hover"]["isHovering"]    = nullptr;
        Viewport["hover"]["localPhysical"] = nullptr;
    } else {
        const auto& Bounds     = *Source.EditorData.PrimaryViewport;
        const bool  IsDrawable = Bounds.Width > 0 && Bounds.Height > 0;
        CommandJson Hover;
        if (!Source.IO)
            Hover = Unavailable("imgui_context_unavailable");
        else if (!IsDrawable)
            Hover = Unavailable("zero_sized_viewport");
        else
            Hover = CommandJson{{"status", "available"}, {"reason", nullptr}};
        Hover["isHovering"] =
            Hover["status"] == "available" ? CommandJson(Source.EditorData.IsHovering) : CommandJson(nullptr);
        Hover["localPhysical"] = Source.IO && Source.EditorData.IsHovering && IsDrawable
                                     ? CommandJson{{"x", Source.EditorData.HoverPixel.X},
                                                   {"y", Source.EditorData.HoverPixel.Y},
                                                   {"units", "physical_pixels"}}
                                     : CommandJson(nullptr);
        Viewport               = CommandJson{{"status", "available"},
                                             {"reason", nullptr},
                                             {"physical", ViewportRect(Bounds, Source.IO, false)},
                                             {"logical", ViewportRect(Bounds, Source.IO, true)},
                                             {"isDrawable", IsDrawable},
                                             {"coordinateOrigin", "top_left"},
                                             {"hover", std::move(Hover)}};
    }

    const bool  HasFocus = Source.IO && Source.Window;
    CommandJson Focus =
        HasFocus ? CommandJson{{"status", "available"},
                               {"reason", nullptr},
                               {"source", "imgui"},
                               {"isFocused", !Source.IO->AppFocusLost}}
                 : CommandJson{{"status", "unavailable"},
                               {"reason", Source.IO ? "window_context_unavailable" : "imgui_context_unavailable"},
                               {"source", "imgui"},
                               {"isFocused", nullptr}};
    CommandJson Capture = Source.IO ? CommandJson{{"status", "available"},
                                                  {"reason", nullptr},
                                                  {"mouse", Source.IO->WantCaptureMouse},
                                                  {"keyboard", Source.IO->WantCaptureKeyboard},
                                                  {"textInput", Source.IO->WantTextInput}}
                                    : CommandJson{{"status", "unavailable"},
                                                  {"reason", "imgui_context_unavailable"},
                                                  {"mouse", nullptr},
                                                  {"keyboard", nullptr},
                                                  {"textInput", nullptr}};
    CommandJson Input{
        {"focus", std::move(Focus)},
        {"capture", std::move(Capture)},
        {"ownership", {{"status", "unavailable"}, {"reason", "input_ownership_unavailable"}, {"owner", nullptr}}},
    };

    CommandJson Panels{
        {"desired", Source.IO ? BoundedPanels(Source.DesiredPanels) : UnavailablePanels("imgui_context_unavailable")},
        {"drawn", Source.IO ? BoundedPanels(Source.DrawnPanels) : UnavailablePanels("imgui_context_unavailable")},
    };

    CommandJson Selection{{"identityScope", "scene_local_unstable"}, {"readback", CollectReadback(Source)}};
    if (!Source.SceneValue) {
        Selection["status"] = "unavailable";
        Selection["reason"] = "scene_context_unavailable";
        Selection["entity"] = nullptr;
    } else if (Source.EditorData.SelectedEntity &&
               !Source.SceneValue->GetRegistry().valid(*Source.EditorData.SelectedEntity)) {
        Selection["status"] = "unavailable";
        Selection["reason"] = "selected_entity_not_in_scene";
        Selection["entity"] = nullptr;
    } else {
        Selection["status"] = "available";
        Selection["reason"] = nullptr;
        Selection["entity"] = EntityJson(Source.EditorData.SelectedEntity);
    }

    return CommandJson{{"schemaVersion", 1},
                       {"frame", Source.Frame},
                       {"phase", "post_update"},
                       {"lifecycle", LifecycleName(Source.Lifecycle)},
                       {"application", std::move(ApplicationState)},
                       {"renderer", std::move(RendererState)},
                       {"window", std::move(Window)},
                       {"viewport", std::move(Viewport)},
                       {"input", std::move(Input)},
                       {"panels", std::move(Panels)},
                       {"selection", std::move(Selection)}};
}

} // namespace SoulEngine
