module;

#include <entt/entt.hpp>

module Scene:YamlIO;

import Scene;
import Resource;

namespace SoulEngine {

namespace {

[[nodiscard]] auto MakeStructuralError(const YamlNode& Node, StringView Message) -> std::unexpected<ErrorMessage> {
    return std::unexpected(ErrorMessage(Format("Scene document at {}: {}", Node.Location(), Message)));
}

auto AppendComponentWarning(SceneLoadReport& Report, String Location, String Message) -> void {
    Report.Warnings.emplace_back(ComponentWarning{
        .Location = std::move(Location),
        .Message  = std::move(Message),
    });
}

auto LoadComponents(Scene& Scene, entt::entity Entity, const YamlNode& Node, SceneLoadReport& Report) -> void {
    if (Node.IsNull())
        return;
    if (!Node.IsMap()) {
        AppendComponentWarning(Report, Node.Location(), "components must be a mapping; all components were omitted");
        return;
    }

    auto& Registry = Scene.GetRegistry();
    for (const auto& [ComponentName, ComponentNodePtr] : Node.Items()) {
        const auto& ComponentNode = *ComponentNodePtr;
        if (!ComponentNode.IsMap()) {
            AppendComponentWarning(Report, ComponentNode.Location(), "component data must be a mapping; component was omitted");
            continue;
        }

        const auto Type = entt::resolve(entt::hashed_string{ComponentName.c_str(), ComponentName.size()}.value());
        if (!Type) {
            AppendComponentWarning(Report, ComponentNode.Location(), "unknown component; component was omitted");
            continue;
        }
        auto Component = Type.construct();
        if (!Component) {
            AppendComponentWarning(Report, ComponentNode.Location(), "component could not be default constructed");
            continue;
        }
        bool Valid = true;
        for (const auto& [FieldName, FieldValuePtr] : ComponentNode.Items()) {
            const auto& FieldValue = *FieldValuePtr;
            const auto Field = Type.data(entt::hashed_string{FieldName.c_str(), FieldName.size()}.value());
            if (!Field) {
                AppendComponentWarning(Report, FieldValue.Location(), "unknown component field");
                Valid = false;
                break;
            }
            if (auto Result = AssignYamlValue(Field, Component, FieldValue); !Result) {
                AppendComponentWarning(Report, FieldValue.Location(), Result.error().ToString());
                Valid = false;
                break;
            }
        }

        if (!Valid)
            continue;

        const auto Emplace = Type.func(entt::hashed_string{"emplace"}.value());
        if (!Emplace || !Emplace.invoke({}, &Registry, Entity, Component))
            AppendComponentWarning(Report, ComponentNode.Location(), "could not emplace component through metadata");
    }
}

[[nodiscard]] auto
LoadEntity(Scene& Scene, const YamlNode& Node, entt::entity Parent, SceneLoadReport& Report)
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

} // namespace

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
