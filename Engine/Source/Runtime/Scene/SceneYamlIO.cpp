module;

#include <entt/entt.hpp>

module Scene.World;

import Core;
import Scene.Camera;
import Scene.Mesh;
import Scene.Light;

namespace SoulEngine {

[[nodiscard]] auto MakeStructuralError(const YamlNode& Node, StringView Message) -> std::unexpected<ErrorMessage> {
    return std::unexpected(ErrorMessage(Format("Scene document at {}: {}", Node.Location(), Message)));
}

auto AppendComponentWarning(SceneLoadReport& Report, String Location, String Message) -> void {
    Report.Warnings.emplace_back(ComponentWarning{
        .Location = std::move(Location),
        .Message  = std::move(Message),
    });
}

template <typename T>
[[nodiscard]] auto DescribeComponent(StringView Name, FieldTable Fields) -> SceneComponentDescriptor {
    return SceneComponentDescriptor{
        .Name           = Name,
        .Fields         = Fields,
        .EmplaceDefault = [](entt::registry& Registry, entt::entity Entity) -> void* {
            return &Registry.emplace_or_replace<T>(Entity, T{});
        },
        .Remove = [](entt::registry& Registry, entt::entity Entity) { Registry.remove<T>(Entity); },
    };
}

auto SceneComponentDescriptors() -> std::span<const SceneComponentDescriptor> {
    static const std::array Table = {
        DescribeComponent<TransformComponent>("transform", TransformComponent::Fields),
        DescribeComponent<CameraComponent>("camera", CameraComponent::Fields),
        DescribeComponent<LightComponent>("light", LightComponent::Fields),
        DescribeComponent<MeshComponent>("mesh", MeshComponent::Fields),
    };
    return Table;
}

auto LoadComponents(Scene& Scene, entt::entity Entity, const YamlNode& Node, SceneLoadReport& Report) -> void {
    if (Node.IsNull())
        return;
    if (!Node.IsMap()) {
        AppendComponentWarning(Report, Node.Location(), "components must be a mapping; all components were omitted");
        return;
    }

    auto&      Registry    = Scene.GetRegistry();
    const auto Descriptors = SceneComponentDescriptors();
    for (const auto& [ComponentName, ComponentNodePtr] : Node.Items()) {
        const auto& ComponentNode = *ComponentNodePtr;
        if (!ComponentNode.IsMap()) {
            AppendComponentWarning(
                Report, ComponentNode.Location(), "component data must be a mapping; component was omitted");
            continue;
        }

        const auto Descriptor =
            std::ranges::find_if(Descriptors, [ComponentName](const SceneComponentDescriptor& Entry) {
                return Entry.Name == ComponentName;
            });
        if (Descriptor == Descriptors.end()) {
            AppendComponentWarning(Report, ComponentNode.Location(), "unknown component; component was omitted");
            continue;
        }

        void* Component = Descriptor->EmplaceDefault(Registry, Entity);
        bool  Valid     = true;
        for (const auto& [FieldName, FieldNodePtr] : ComponentNode.Items()) {
            const auto& FieldNode = *FieldNodePtr;
            const auto* Field     = FindField(Descriptor->Fields, FieldName);
            if (!Field) {
                AppendComponentWarning(Report, FieldNode.Location(), "unknown component field");
                Valid = false;
                break;
            }
            // Invalid values keep the component's declared default for that field; unknown fields omit the
            // component entirely.
            if (auto Result = AssignYamlField(*Field, Component, FieldNode); !Result)
                AppendComponentWarning(Report, FieldNode.Location(), Result.error().ToString());
        }

        if (!Valid)
            Descriptor->Remove(Registry, Entity);
    }
}

[[nodiscard]] auto LoadEntity(Scene& Scene, const YamlNode& Node, entt::entity Parent, SceneLoadReport& Report)
    -> std::expected<void, ErrorMessage> {
    if (!Node.IsMap())
        return MakeStructuralError(Node, "entity must be a mapping");

    for (const auto& [Key, ValuePtr] : Node.Items()) {
        if (Key != "name" && Key != "components" && Key != "children")
            return MakeStructuralError(*ValuePtr, "is not a recognized entity field");
    }

    String Name = {};
    if (const auto NameNode = Node["name"]) {
        const auto& NameValue = NameNode->get();
        if (!NameValue.IsScalar())
            return MakeStructuralError(NameValue, "must be a scalar string");
        Name = NameValue.Scalar();
    }

    const auto Entity = Name.empty() ? Scene.CreateEntity(Parent) : Scene.CreateEntityWithName(std::move(Name), Parent);
    if (const auto Components = Node["components"])
        LoadComponents(Scene, Entity, Components->get(), Report);

    const auto Children = Node["children"];
    if (!Children || Children->get().IsNull())
        return {};
    if (!Children->get().IsSequence())
        return MakeStructuralError(Children->get(), "must be a sequence");
    for (const YamlNode& Child : Children->get().Values()) {
        if (auto Result = LoadEntity(Scene, Child, Entity, Report); !Result)
            return std::unexpected(Result.error());
    }
    return {};
}

[[nodiscard]] auto Scene::LoadFromFile(const Path& FilePath)
    -> std::expected<std::pair<UPtr<Scene>, SceneLoadReport>, ErrorMessage> {
    auto Document = YamlDocument::Create(FilePath);
    if (!Document)
        return std::unexpected(Document.error());

    // Structural errors carry the file name on top of the node location.
    const auto Fail = [&FilePath](const YamlNode& Node, StringView Message) -> std::unexpected<ErrorMessage> {
        return std::unexpected(MakeStructuralError(Node, Message)
                                   .error()
                                   .Append(Format("Failed to load Scene document '{}'", FilePath.string())));
    };

    const auto& Root = Document->Root();
    if (!Root.IsMap())
        return Fail(Root, "document root must be a mapping");

    for (const auto& [Key, ValuePtr] : Root.Items()) {
        if (Key != "entities")
            return Fail(*ValuePtr, "is not a recognized Scene File field");
    }

    const auto Entities = Root["entities"];
    if (!Entities || !Entities->get().IsSequence())
        return Fail(Entities ? Entities->get() : Root, "must be a sequence");

    auto Temporary = std::make_unique<Scene>();
    Temporary->SetAssetRoot((FilePath.parent_path() / "Assets").lexically_normal());
    SceneLoadReport Report = {};
    for (const YamlNode& Entity : Entities->get().Values()) {
        if (auto Result = LoadEntity(*Temporary, Entity, entt::null, Report); !Result)
            return std::unexpected(
                Result.error().Append(Format("Failed to load Scene document '{}'", FilePath.string())));
    }
    return std::pair{std::move(Temporary), std::move(Report)};
}

} // namespace SoulEngine
