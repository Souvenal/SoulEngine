module;

#include <entt/entt.hpp>
#include <hlsl++.h>
#include <yaml-cpp/yaml.h>

module Scene:YamlIO;

import Scene;

using namespace SoulEngine::Core;

namespace SoulEngine::Scene {

namespace {

struct SceneFieldSchema {
    StringView Name = {};
    entt::id_type Id = {};
};

struct SceneComponentSchema {
    entt::meta_any (*Create)(entt::registry&, SceneEntity);
    void (*Remove)(entt::registry&, SceneEntity);
    [[nodiscard]] auto (*Has)(const entt::registry&, SceneEntity) -> bool;
    [[nodiscard]] entt::meta_any (*Get)(entt::registry&, SceneEntity);
    [[nodiscard]] auto (*Validate)(const entt::meta_any&, String&) -> bool;
    StringView Name = {};
    std::span<const SceneFieldSchema> Fields = {};
};

template <typename T>
[[nodiscard]] auto CreateComponent(entt::registry& Registry, SceneEntity Entity) -> entt::meta_any {
    return entt::forward_as_meta(Registry.emplace<T>(Entity));
}

template <typename T>
auto RemoveComponent(entt::registry& Registry, SceneEntity Entity) -> void {
    Registry.remove<T>(Entity);
}

template <typename T>
[[nodiscard]] auto HasComponent(const entt::registry& Registry, SceneEntity Entity) -> bool {
    return Registry.all_of<T>(Entity);
}

template <typename T>
[[nodiscard]] auto GetComponent(entt::registry& Registry, SceneEntity Entity) -> entt::meta_any {
    return entt::forward_as_meta(Registry.get<T>(Entity));
}

[[nodiscard]] auto ValidateCamera(const entt::meta_any& Value, String& Error) -> bool {
    const auto* Camera = Value.try_cast<CameraComponent>();
    if (!Camera) {
        Error = "Camera component metadata does not contain CameraComponent";
        return false;
    }
    if (Camera->FOV <= 0.0f || Camera->FOV >= 179.0f) {
        Error = "fov_degrees must be in the range (0, 179)";
        return false;
    }
    if (Camera->NearPlane <= 0.0f) {
        Error = "near_plane must be greater than zero";
        return false;
    }
    if (Camera->FarPlane <= Camera->NearPlane) {
        Error = "far_plane must be greater than near_plane";
        return false;
    }
    return true;
}

[[nodiscard]] auto ValidateMesh(const entt::meta_any& Value, String& Error) -> bool {
    const auto* Mesh = Value.try_cast<MeshComponent>();
    if (!Mesh) {
        Error = "Mesh component metadata does not contain MeshComponent";
        return false;
    }
    if (Mesh->Asset.empty()) {
        Error = "asset must not be empty";
        return false;
    }
    if (Path(Mesh->Asset).is_absolute()) {
        Error = "asset must be relative to the current application Assets directory";
        return false;
    }
    return true;
}

auto RegisterBuiltInComponentSchemas() -> void {
    static bool Registered = false;
    if (Registered)
        return;

    static const std::array CameraFields{
        SceneFieldSchema{.Name = "fov_degrees", .Id = entt::hashed_string{"fov_degrees"}.value()},
        SceneFieldSchema{.Name = "near_plane", .Id = entt::hashed_string{"near_plane"}.value()},
        SceneFieldSchema{.Name = "far_plane", .Id = entt::hashed_string{"far_plane"}.value()},
    };
    static const std::array MeshFields{
        SceneFieldSchema{.Name = "asset", .Id = entt::hashed_string{"asset"}.value()},
    };

    entt::meta_factory<CameraComponent>{}
        .type(entt::hashed_string{"camera"}.value())
        .custom<SceneComponentSchema>(SceneComponentSchema{
            .Create = &CreateComponent<CameraComponent>, .Remove = &RemoveComponent<CameraComponent>,
            .Has = &HasComponent<CameraComponent>, .Get = &GetComponent<CameraComponent>,
            .Validate = &ValidateCamera, .Name = "camera", .Fields = CameraFields,
        })
        .data<&CameraComponent::FOV>(CameraFields[0].Id)
        .data<&CameraComponent::NearPlane>(CameraFields[1].Id)
        .data<&CameraComponent::FarPlane>(CameraFields[2].Id);

    entt::meta_factory<MeshComponent>{}
        .type(entt::hashed_string{"mesh"}.value())
        .custom<SceneComponentSchema>(SceneComponentSchema{
            .Create = &CreateComponent<MeshComponent>, .Remove = &RemoveComponent<MeshComponent>,
            .Has = &HasComponent<MeshComponent>, .Get = &GetComponent<MeshComponent>,
            .Validate = &ValidateMesh, .Name = "mesh", .Fields = MeshFields,
        })
        .data<&MeshComponent::Asset>(MeshFields[0].Id);

    Registered = true;
}
[[nodiscard]] auto MakeStructuralError(StringView Path, StringView Message) -> std::unexpected<ErrorMessage> {
    return std::unexpected(ErrorMessage(Format("Scene document at '{}': {}", Path, Message)));
}

[[nodiscard]] auto MakeYamlPath(StringView Parent, StringView Child) -> String {
    if (Parent.empty())
        return String(Child);
    return Format("{}.{}", Parent, Child);
}

[[nodiscard]] auto ReadFloat3(const YAML::Node& Node, StringView Path) -> std::expected<hlslpp::float3, ErrorMessage> {
    if (!Node.IsSequence() || Node.size() != 3)
        return MakeStructuralError(Path, "must be a sequence of exactly three numbers");

    try {
        return hlslpp::float3(Node[0].as<float>(), Node[1].as<float>(), Node[2].as<float>());
    } catch (const YAML::Exception& Error) {
        return MakeStructuralError(Path, Error.what());
    }
}

[[nodiscard]] auto ReadTransform(const YAML::Node& Node, StringView Path) -> std::expected<Transform, ErrorMessage> {
    Transform Result = {};
    if (!Node.IsDefined() || Node.IsNull())
        return Result;
    if (!Node.IsMap())
        return MakeStructuralError(Path, "must be a mapping");

    for (const auto& Entry : Node) {
        if (!Entry.first.IsScalar())
            return MakeStructuralError(Path, "contains a non-scalar key");
        const auto Key = Entry.first.as<String>();
        const auto FieldPath = MakeYamlPath(Path, Key);
        if (Key == "translation") {
            auto Value = ReadFloat3(Entry.second, FieldPath);
            if (!Value)
                return std::unexpected(Value.error());
            Result.Translation = Value.value();
        } else if (Key == "rotation_degrees") {
            auto Value = ReadFloat3(Entry.second, FieldPath);
            if (!Value)
                return std::unexpected(Value.error());
            Result.RotationDegrees = Value.value();
        } else if (Key == "scale") {
            auto Value = ReadFloat3(Entry.second, FieldPath);
            if (!Value)
                return std::unexpected(Value.error());
            Result.Scale = Value.value();
        } else {
            return MakeStructuralError(FieldPath, "is not a recognized Transform field");
        }
    }
    return Result;
}

[[nodiscard]] auto AssignYamlValue(const entt::meta_data& Field,
                                   entt::meta_any& Component,
                                   const YAML::Node& Value,
                                   String& Error) -> bool {
    try {
        const auto Type = Field.type().id();
        if (Type == entt::type_hash<float>::value()) {
            if (!Value.IsScalar()) {
                Error = "must be a scalar number";
                return false;
            }
            if (!Field.set(Component, Value.as<float>())) {
                Error = "could not assign numeric value through metadata";
                return false;
            }
            return true;
        }
        if (Type == entt::type_hash<String>::value()) {
            if (!Value.IsScalar()) {
                Error = "must be a scalar string";
                return false;
            }
            if (!Field.set(Component, Value.as<String>())) {
                Error = "could not assign string value through metadata";
                return false;
            }
            return true;
        }
        Error = "uses an unsupported persisted field type";
        return false;
    } catch (const YAML::Exception& Exception) {
        Error = Exception.what();
        return false;
    }
}

[[nodiscard]] auto AppendComponentWarning(SceneLoadReport& Report, String Path, String Message) -> void {
    Report.Warnings.emplace_back(ComponentWarning{
        .Path    = std::move(Path),
        .Message = std::move(Message),
    });
}

auto LoadComponents(Scene& Scene, SceneEntity Entity, const YAML::Node& Node, StringView Path, SceneLoadReport& Report)
    -> void {
    if (!Node.IsDefined() || Node.IsNull())
        return;
    if (!Node.IsMap()) {
        AppendComponentWarning(Report, String(Path), "components must be a mapping; all components were omitted");
        return;
    }

    auto& Registry = Scene.GetRegistry();
    for (const auto& Entry : Node) {
        if (!Entry.first.IsScalar()) {
            AppendComponentWarning(Report, String(Path), "component name must be a scalar; component was omitted");
            continue;
        }
        const auto ComponentName = Entry.first.as<String>();
        const auto ComponentPath = MakeYamlPath(Path, ComponentName);
        if (!Entry.second.IsMap()) {
            AppendComponentWarning(Report, ComponentPath, "component data must be a mapping; component was omitted");
            continue;
        }

        const auto Type = entt::resolve(entt::hashed_string{ComponentName.c_str(), ComponentName.size()}.value());
        const auto* Schema = static_cast<SceneComponentSchema*>(Type.custom());
        if (!Type || !Schema) {
            AppendComponentWarning(Report, ComponentPath, "unknown component; component was omitted");
            continue;
        }

        auto Component = Schema->Create(Registry, Entity);
        bool Valid = true;
        for (const auto& FieldEntry : Entry.second) {
            if (!FieldEntry.first.IsScalar()) {
                AppendComponentWarning(Report, ComponentPath, "component field name must be a scalar");
                Valid = false;
                break;
            }
            const auto FieldName = FieldEntry.first.as<String>();
            const auto FieldPath = MakeYamlPath(ComponentPath, FieldName);
            const auto Field = Type.data(entt::hashed_string{FieldName.c_str(), FieldName.size()}.value());
            if (!Field) {
                AppendComponentWarning(Report, FieldPath, "unknown component field");
                Valid = false;
                break;
            }
            String Error;
            if (!AssignYamlValue(Field, Component, FieldEntry.second, Error)) {
                AppendComponentWarning(Report, FieldPath, std::move(Error));
                Valid = false;
                break;
            }
        }

        String Error;
        if (Valid && !Schema->Validate(Component, Error)) {
            AppendComponentWarning(Report, ComponentPath, std::move(Error));
            Valid = false;
        }
        if (!Valid)
            Schema->Remove(Registry, Entity);
    }
}

[[nodiscard]] auto LoadEntity(Scene& Scene,
                              const YAML::Node& Node,
                              SceneEntity Parent,
                              StringView Path,
                              SceneLoadReport& Report) -> std::expected<void, ErrorMessage> {
    if (!Node.IsMap())
        return MakeStructuralError(Path, "entity must be a mapping");

    String Name = {};
    Transform LocalTransform = {};
    YAML::Node Components = {};
    YAML::Node Children = {};
    for (const auto& Entry : Node) {
        if (!Entry.first.IsScalar())
            return MakeStructuralError(Path, "entity contains a non-scalar key");
        const auto Key = Entry.first.as<String>();
        const auto FieldPath = MakeYamlPath(Path, Key);
        if (Key == "name") {
            if (!Entry.second.IsScalar())
                return MakeStructuralError(FieldPath, "must be a scalar string");
            try {
                Name = Entry.second.as<String>();
            } catch (const YAML::Exception& Error) {
                return MakeStructuralError(FieldPath, Error.what());
            }
        } else if (Key == "transform") {
            auto TransformResult = ReadTransform(Entry.second, FieldPath);
            if (!TransformResult)
                return std::unexpected(TransformResult.error());
            LocalTransform = TransformResult.value();
        } else if (Key == "components") {
            Components = Entry.second;
        } else if (Key == "children") {
            Children = Entry.second;
        } else {
            return MakeStructuralError(FieldPath, "is not a recognized entity field");
        }
    }

    const auto Entity = Scene.CreateEntity(std::move(Name), Parent);
    Scene.GetRegistry().get<SceneNode>(Entity).Transform = LocalTransform;
    LoadComponents(Scene, Entity, Components, MakeYamlPath(Path, "components"), Report);

    if (!Children.IsDefined() || Children.IsNull())
        return {};
    if (!Children.IsSequence())
        return MakeStructuralError(MakeYamlPath(Path, "children"), "must be a sequence");
    for (std::size_t Index = 0; Index < Children.size(); ++Index) {
        if (auto Result = LoadEntity(Scene, Children[Index], Entity, Format("{}.children[{}]", Path, Index), Report); !Result)
            return std::unexpected(Result.error());
    }
    return {};
}

[[nodiscard]] auto SaveFloat3(const hlslpp::float3& Value) -> YAML::Node {
    YAML::Node Result{YAML::NodeType::Sequence};
    Result.push_back(static_cast<float>(Value.x));
    Result.push_back(static_cast<float>(Value.y));
    Result.push_back(static_cast<float>(Value.z));
    return Result;
}

[[nodiscard]] auto SaveYamlValue(const entt::meta_data& Field, const entt::meta_any& Component)
    -> std::expected<YAML::Node, ErrorMessage> {
    const auto Value = Field.get(Component);
    if (!Value)
        return std::unexpected(ErrorMessage("Scene component metadata could not read a persisted field"));
    if (const auto* Float = Value.try_cast<float>())
        return YAML::Node(*Float);
    if (const auto* StringValue = Value.try_cast<String>())
        return YAML::Node(*StringValue);
    return std::unexpected(ErrorMessage("Scene component metadata contains an unsupported persisted field type"));
}

[[nodiscard]] auto SaveEntity(const Scene& Scene, SceneEntity Entity) -> std::expected<YAML::Node, ErrorMessage> {
    const auto& Registry = Scene.GetRegistry();
    const auto& Node = Registry.get<SceneNode>(Entity);
    YAML::Node Result{YAML::NodeType::Map};
    if (!Node.Name.empty())
        Result["name"] = Node.Name;

    YAML::Node TransformNode{YAML::NodeType::Map};
    TransformNode["translation"] = SaveFloat3(Node.Transform.Translation);
    TransformNode["rotation_degrees"] = SaveFloat3(Node.Transform.RotationDegrees);
    TransformNode["scale"] = SaveFloat3(Node.Transform.Scale);
    Result["transform"] = TransformNode;

    YAML::Node Components{YAML::NodeType::Map};
    for (const auto& [UnusedId, Type] : entt::resolve()) {
        const auto* Schema = static_cast<SceneComponentSchema*>(Type.custom());
        if (!Schema || !Schema->Has(Registry, Entity))
            continue;

        YAML::Node ComponentNode{YAML::NodeType::Map};
        const auto Component = Schema->Get(const_cast<entt::registry&>(Registry), Entity);
        for (const auto& FieldSchema : Schema->Fields) {
            const auto Field = Type.data(FieldSchema.Id);
            auto Value = SaveYamlValue(Field, Component);
            if (!Value)
                return std::unexpected(Value.error().Append(Format("Failed to save component '{}'", Schema->Name)));
            ComponentNode[String(FieldSchema.Name)] = Value.value();
        }
        Components[String(Schema->Name)] = ComponentNode;
    }
    if (Components.size() != 0)
        Result["components"] = Components;

    if (!Node.Children.empty()) {
        YAML::Node Children{YAML::NodeType::Sequence};
        for (const auto Child : Node.Children) {
            auto SavedChild = SaveEntity(Scene, Child);
            if (!SavedChild)
                return std::unexpected(SavedChild.error());
            Children.push_back(SavedChild.value());
        }
        Result["children"] = Children;
    }
    return Result;
}

} // namespace

[[nodiscard]] auto Scene::LoadFromFile(const Path& FilePath) -> std::expected<SceneLoadReport, ErrorMessage> {
    RegisterBuiltInComponentSchemas();

    YAML::Node Document;
    try {
        Document = YAML::LoadFile(FilePath.string());
    } catch (const YAML::Exception& Error) {
        return std::unexpected(ErrorMessage(Format("Failed to parse Scene document '{}': {}", FilePath.string(), Error.what())));
    }
    if (!Document.IsMap())
        return MakeStructuralError(FilePath.string(), "document root must be a mapping");

    YAML::Node Entities = {};
    for (const auto& Entry : Document) {
        if (!Entry.first.IsScalar())
            return MakeStructuralError(FilePath.string(), "document contains a non-scalar key");
        const auto Key = Entry.first.as<String>();
        if (Key != "entities")
            return MakeStructuralError(MakeYamlPath(FilePath.string(), Key), "is not a recognized Scene File field");
        Entities = Entry.second;
    }
    if (!Entities || !Entities.IsSequence())
        return MakeStructuralError(MakeYamlPath(FilePath.string(), "entities"), "must be a sequence");

    Scene Temporary = {};
    SceneLoadReport Report = {};
    for (std::size_t Index = 0; Index < Entities.size(); ++Index) {
        if (auto Result = LoadEntity(Temporary, Entities[Index], entt::null, Format("entities[{}]", Index), Report); !Result)
            return std::unexpected(Result.error().Append(Format("Failed to load Scene document '{}'", FilePath.string())));
    }
    Temporary.UpdateWorldTransforms();
    *this = std::move(Temporary);
    return Report;
}

[[nodiscard]] auto Scene::SaveToFile(const Path& FilePath) const -> std::expected<void, ErrorMessage> {
    RegisterBuiltInComponentSchemas();

    YAML::Node Document{YAML::NodeType::Map};
    YAML::Node Entities{YAML::NodeType::Sequence};
    for (const auto Root : m_Roots) {
        auto SavedRoot = SaveEntity(*this, Root);
        if (!SavedRoot)
            return std::unexpected(SavedRoot.error().Append(Format("Failed to save Scene document '{}'", FilePath.string())));
        Entities.push_back(SavedRoot.value());
    }
    Document["entities"] = Entities;

    YAML::Emitter Emitter;
    Emitter << Document;
    if (!Emitter.good())
        return std::unexpected(ErrorMessage(Format("Failed to emit Scene document '{}': {}", FilePath.string(), Emitter.GetLastError())));

    std::ofstream Output(FilePath);
    if (!Output)
        return std::unexpected(ErrorMessage(Format("Failed to open Scene document '{}' for writing", FilePath.string())));
    Output << Emitter.c_str();
    if (!Output)
        return std::unexpected(ErrorMessage(Format("Failed to write Scene document '{}'", FilePath.string())));
    return {};
}

} // namespace SoulEngine::Scene








