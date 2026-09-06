module;
#include <hlsl++.h>
#include <imgui.h>

export module Editor:UIPanels;

import std;
import magic_enum;
import Core;
import Material;
import Scene;
import :UIManager;

export namespace SoulEngine {

/// Why a scene provider? Because scene may change at runtime,
/// and UI doen't depend on a specific scene.
///
/// @brief Point the debug panels at the active scene's material store.
auto RegisterAllUI(std::function<const Scene*()> SceneProvider) -> void {
    // Materials Window
    UIManager::Get().Register(
        "Debug.ShowMaterials",
        [SceneProvider = std::move(SceneProvider),
         MaterialListWidth = 220.0F,
         SelectedScene = static_cast<const Scene*>(nullptr),
         SelectedMaterialId = Uint64{0}]() mutable {
            auto& entry = UIManager::Get().GetAll().at("Debug.ShowMaterials");
            if (ImGui::Begin("Materials", &entry.Show)) {
                const auto* SceneValue = SceneProvider();
                if (SceneValue != SelectedScene) {
                    SelectedScene      = SceneValue;
                    SelectedMaterialId = 0;
                }
                const auto Meshes = SceneValue
                                        ? SceneValue->GetSystem<MeshSystem>()
                                        : std::optional<std::reference_wrapper<const MeshSystem>>{};
                if (!Meshes) {
                    ImGui::TextUnformatted("No active Scene mesh system.");
                } else {
                    auto Materials = Meshes->get().CollectMaterialRecords();
                    ImGui::Text("Material records: %zu", Materials.size());
                    ImGui::Separator();
                    const auto DisplayFloat3 = [](const char* Label, const hlslpp::float3& Value) -> void {
                        ImGui::Text("%s: (%.3f, %.3f, %.3f)",
                                    Label,
                                    static_cast<float>(Value.x),
                                    static_cast<float>(Value.y),
                                    static_cast<float>(Value.z));
                    };
                    const auto DisplayFloat4 = [](const char* Label, const hlslpp::float4& Value) -> void {
                        ImGui::Text("%s: (%.3f, %.3f, %.3f, %.3f)",
                                    Label,
                                    static_cast<float>(Value.x),
                                    static_cast<float>(Value.y),
                                    static_cast<float>(Value.z),
                                    static_cast<float>(Value.w));
                    };

                    const auto MaterialIdentity = [](const ConstMaterialHandle& Material) -> Uint64 {
                        return Material ? static_cast<Uint64>(reinterpret_cast<std::uintptr_t>(std::addressof(*Material)))
                                        : 0;
                    };
                    const auto RelativeSourcePath = [SceneValue](const MaterialRecord& Material) -> String {
                        if (Material.SourceAsset.empty())
                            return {};
                        return Material.SourceAsset.lexically_relative(SceneValue->GetAssetRoot()).generic_string();
                    };
                    const auto FindMaterial = [&Materials, &MaterialIdentity](Uint64 Identity)
                        -> const ConstMaterialHandle* {
                        for (const auto& Material : Materials)
                            if (MaterialIdentity(Material) == Identity)
                                return &Material;
                        return nullptr;
                    };
                    const auto* FirstMaterial = Materials.empty() ? nullptr : &Materials.front();

                    const auto AvailableSize = ImGui::GetContentRegionAvail();
                    constexpr float kMinimumListWidth   = 180.0F;
                    constexpr float kMinimumDetailsWidth = 240.0F;
                    constexpr float kSplitterWidth       = 6.0F;
                    const float MaximumListWidth = std::max(
                        kMinimumListWidth,
                        AvailableSize.x - kMinimumDetailsWidth - kSplitterWidth);
                    MaterialListWidth = std::clamp(MaterialListWidth, kMinimumListWidth, MaximumListWidth);

                    ImGui::BeginChild("MaterialList", ImVec2(MaterialListWidth, AvailableSize.y), true);
                    for (std::size_t GroupBegin = 0; GroupBegin < Materials.size();) {
                        const auto& FirstInGroup = Materials[GroupBegin];
                        const auto  Source       = FirstInGroup->Source;
                        const auto  SourcePath   = RelativeSourcePath(*FirstInGroup);
                        auto        GroupEnd     = GroupBegin + 1;
                        while (GroupEnd < Materials.size() && Materials[GroupEnd]->Source == Source &&
                               (Source == MaterialSource::Yaml ||
                                Materials[GroupEnd]->SourceAsset == FirstInGroup->SourceAsset))
                            ++GroupEnd;

                        ImGui::PushID(Source == MaterialSource::Yaml ? "YAML Materials" : SourcePath.c_str());
                        const auto GroupLabel = Source == MaterialSource::Yaml   ? String{"YAML Materials"}
                                                : Source == MaterialSource::Assimp ? SourcePath
                                                                                   : String{"Unknown Materials"};
                        if (ImGui::TreeNodeEx(GroupLabel.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                            for (std::size_t MaterialIndex = GroupBegin; MaterialIndex < GroupEnd; ++MaterialIndex) {
                                const auto& Material = Materials[MaterialIndex];
                                ImGui::PushID(std::addressof(*Material));
                                const bool IsSelected = MaterialIdentity(Material) == SelectedMaterialId;
                                if (ImGui::Selectable(Material->Name.c_str(), IsSelected))
                                    SelectedMaterialId = MaterialIdentity(Material);
                                ImGui::PopID();
                            }
                            ImGui::TreePop();
                        }
                        ImGui::PopID();
                        GroupBegin = GroupEnd;
                    }
                    ImGui::EndChild();

                    ImGui::SameLine(0.0F, 0.0F);
                    ImGui::InvisibleButton("MaterialListSplitter", ImVec2(kSplitterWidth, AvailableSize.y));
                    if (ImGui::IsItemHovered() || ImGui::IsItemActive())
                        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                    if (ImGui::IsItemActive())
                        MaterialListWidth = std::clamp(MaterialListWidth + ImGui::GetIO().MouseDelta.x,
                                                       kMinimumListWidth,
                                                       MaximumListWidth);

                    const auto* SelectedMaterialHandle = FindMaterial(SelectedMaterialId);
                    if (!SelectedMaterialHandle && FirstMaterial) {
                        SelectedMaterialId = MaterialIdentity(*FirstMaterial);
                        SelectedMaterialHandle = FirstMaterial;
                    }

                    ImGui::SameLine(0.0F, 0.0F);
                    ImGui::BeginChild("MaterialDetails", ImVec2(0.0F, AvailableSize.y), true);
                    if (!SelectedMaterialHandle) {
                        ImGui::TextUnformatted("Select a material to inspect its properties.");
                    } else {
                        const auto& Material = *SelectedMaterialHandle;
                        const auto SourcePath = RelativeSourcePath(*Material);
                        ImGui::Text("Source: %s", SourcePath.c_str());
                        ImGui::Text("Name: %s", Material->Name.c_str());
                        const auto BlendFunction = magic_enum::enum_name(Material->BlendFunction);
                        const auto ShadingModel  = magic_enum::enum_name(Material->ShadingModel);
                        const auto AlphaMode     = magic_enum::enum_name(Material->AlphaMode);
                        ImGui::Text("Blend Function: %.*s",
                                    static_cast<int>(BlendFunction.size()),
                                    BlendFunction.data());
                        ImGui::Text("Shading Model: %.*s",
                                    static_cast<int>(ShadingModel.size()),
                                    ShadingModel.data());
                        ImGui::Text("Alpha Mode: %.*s",
                                    static_cast<int>(AlphaMode.size()),
                                    AlphaMode.data());
                        ImGui::Text("Alpha Cutoff: %.3f", Material->AlphaCutoff);
                        ImGui::Text("Wireframe: %s", Material->EnableWireframe ? "true" : "false");
                        ImGui::Text("Two Sided: %s", Material->TwoSided ? "true" : "false");

                        if (ImGui::CollapsingHeader("PBR", ImGuiTreeNodeFlags_DefaultOpen)) {
                            DisplayFloat4("Base Color Factor", Material->BaseColorFactor);
                            ImGui::Text("Metallic Factor: %.3f", Material->MetallicFactor);
                            ImGui::Text("Roughness Factor: %.3f", Material->RoughnessFactor);
                            DisplayFloat3("Sheen Color Factor", Material->SheenColorFactor);
                            ImGui::Text("Sheen Roughness Factor: %.3f", Material->SheenRoughnessFactor);
                            ImGui::Text("Clearcoat Factor: %.3f", Material->ClearcoatFactor);
                            ImGui::Text("Clearcoat Roughness Factor: %.3f", Material->ClearcoatRoughnessFactor);
                            ImGui::Text("Transmission Factor: %.3f", Material->TransmissionFactor);
                            ImGui::Text("Volume Thickness Factor: %.3f", Material->VolumeThicknessFactor);
                            ImGui::Text("Volume Attenuation Distance: %.3f", Material->VolumeAttenuationDistance);
                            DisplayFloat3("Volume Attenuation Color", Material->VolumeAttenuationColor);
                        }

                        if (ImGui::CollapsingHeader("Legacy Surface")) {
                            DisplayFloat3("Ambient", Material->Ambient);
                            DisplayFloat3("Diffuse", Material->Diffuse);
                            DisplayFloat3("Specular", Material->Specular);
                            DisplayFloat3("Emissive", Material->Emissive);
                            DisplayFloat3("Transparent", Material->Transparent);
                            DisplayFloat3("Reflective", Material->Reflective);
                            ImGui::Text("Opacity: %.3f", Material->Opacity);
                            ImGui::Text("Bump Scaling: %.3f", Material->BumpScaling);
                            ImGui::Text("Shininess: %.3f", Material->Shininess);
                            ImGui::Text("Shininess Strength: %.3f", Material->ShininessStrength);
                            ImGui::Text("Refraction Index: %.3f", Material->RefractionIndex);
                            ImGui::Text("Reflectivity: %.3f", Material->Reflectivity);
                        }

                        if (ImGui::CollapsingHeader("Textures")) {
                            bool HasTextures = false;
                            std::vector<TextureType> TextureTypes = {};
                            for (const auto& Slot : Material->Textures) {
                                if (!std::ranges::contains(TextureTypes, Slot.Type))
                                    TextureTypes.emplace_back(Slot.Type);
                            }
                            HasTextures = !TextureTypes.empty();
                            for (const auto Type : TextureTypes) {
                                const auto TypeName = String{magic_enum::enum_name(Type)};
                                if (ImGui::TreeNode(TypeName.c_str())) {
                                    std::size_t SlotIndex = 0;
                                    for (const auto& Slot : Material->Textures) {
                                        if (Slot.Type != Type)
                                            continue;
                                        ImGui::PushID(static_cast<int>(SlotIndex));
                                        const auto AssetPath = Slot.Texture ? Slot.Texture->AssetPath.string() : "<unresolved>";
                                        ImGui::Text("Asset: %s", AssetPath.c_str());
                                        const auto Mapping   = magic_enum::enum_name(Slot.Mapping);
                                        const auto Operation = magic_enum::enum_name(Slot.Operation);
                                        const auto MapModeU  = magic_enum::enum_name(Slot.MapModeU);
                                        const auto MapModeV  = magic_enum::enum_name(Slot.MapModeV);
                                        const auto MapModeW  = magic_enum::enum_name(Slot.MapModeW);
                                        ImGui::Text("Mapping: %.*s, UV: %u, Blend: %.3f",
                                                    static_cast<int>(Mapping.size()),
                                                    Mapping.data(),
                                                    Slot.UVIndex,
                                                    Slot.Blend);
                                        ImGui::Text("Operation: %.*s, Map Modes: %.*s / %.*s / %.*s",
                                                    static_cast<int>(Operation.size()),
                                                    Operation.data(),
                                                    static_cast<int>(MapModeU.size()),
                                                    MapModeU.data(),
                                                    static_cast<int>(MapModeV.size()),
                                                    MapModeV.data(),
                                                    static_cast<int>(MapModeW.size()),
                                                    MapModeW.data());
                                        ImGui::Text("Flags: 0x%08X", Slot.Flags);
                                        ImGui::Separator();
                                        ImGui::PopID();
                                        ++SlotIndex;
                                    }
                                    ImGui::TreePop();
                                }
                            }
                            if (!HasTextures)
                                ImGui::TextUnformatted("No texture slots.");
                        }
                    }
                    ImGui::EndChild();
                }
            }
            ImGui::End();
        },
        false);

    // 未来在这里添加更多 UI
    // UIManager::Get().Register("xxx", []() { ... }, false);
}

} // namespace SoulEngine
