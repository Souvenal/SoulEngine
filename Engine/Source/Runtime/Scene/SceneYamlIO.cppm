module;

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <entt/entt.hpp>
#include <hlsl++.h>
#include <yaml.h>

module Scene:YamlIO;

import Scene;
import Resource;

namespace SoulEngine {

namespace {

struct YamlDocument {
    yaml_document_t Value       = {};
    bool            Initialized = false;

    ~YamlDocument() {
        if (Initialized)
            yaml_document_delete(&Value);
    }

    YamlDocument()                                       = default;
    YamlDocument(const YamlDocument&)                    = delete;
    auto operator=(const YamlDocument&) -> YamlDocument& = delete;

    YamlDocument(YamlDocument&& Other) noexcept : Value(Other.Value), Initialized(Other.Initialized) {
        Other.Value       = {};
        Other.Initialized = false;
    }

    auto operator=(YamlDocument&& Other) noexcept -> YamlDocument& {
        if (this == &Other)
            return *this;
        if (Initialized)
            yaml_document_delete(&Value);
        Value             = Other.Value;
        Initialized       = Other.Initialized;
        Other.Value       = {};
        Other.Initialized = false;
        return *this;
    }
};

struct YamlNode;

struct YamlMapRange;
struct YamlMapEntry;

struct YamlMapIterator {
    yaml_node_pair_t* Pair     = nullptr;
    yaml_node_pair_t* End      = nullptr;
    yaml_document_t*  Document = nullptr;

    [[nodiscard]] auto operator*() const -> YamlMapEntry;
    auto               operator++() -> YamlMapIterator& {
        ++Pair;
        return *this;
    }
    [[nodiscard]] auto operator!=(const YamlMapIterator& Other) const -> bool {
        return Pair != Other.Pair;
    }
};

struct YamlMapRange {
    YamlMapIterator BeginIterator = {};
    YamlMapIterator EndIterator   = {};

    [[nodiscard]] auto begin() const -> YamlMapIterator {
        return BeginIterator;
    }
    [[nodiscard]] auto end() const -> YamlMapIterator {
        return EndIterator;
    }
};

struct YamlNode {
    yaml_document_t* Document = nullptr;
    int              Id       = 0;

    [[nodiscard]] auto Raw() const -> yaml_node_t* {
        if (!Document || Id == 0)
            return nullptr;
        return yaml_document_get_node(Document, Id);
    }

    [[nodiscard]] auto IsDefined() const -> bool {
        return Raw() != nullptr;
    }

    [[nodiscard]] explicit operator bool() const {
        return IsDefined();
    }

    [[nodiscard]] auto operator!() const -> bool {
        return !IsDefined();
    }

    [[nodiscard]] auto IsNull() const -> bool {
        const auto* Node = Raw();
        if (!Node || Node->type != YAML_SCALAR_NODE || Node->data.scalar.style != YAML_PLAIN_SCALAR_STYLE)
            return false;
        const StringView Value(reinterpret_cast<const char*>(Node->data.scalar.value), Node->data.scalar.length);
        return Value.empty() || Value == "~" || Value == "null" || Value == "Null" || Value == "NULL";
    }

    [[nodiscard]] auto IsScalar() const -> bool {
        const auto* Node = Raw();
        return Node && Node->type == YAML_SCALAR_NODE;
    }

    [[nodiscard]] auto IsSequence() const -> bool {
        const auto* Node = Raw();
        return Node && Node->type == YAML_SEQUENCE_NODE;
    }

    [[nodiscard]] auto IsMap() const -> bool {
        const auto* Node = Raw();
        return Node && Node->type == YAML_MAPPING_NODE;
    }

    [[nodiscard]] auto Scalar() const -> String {
        const auto* Node = Raw();
        if (!Node || Node->type != YAML_SCALAR_NODE)
            return {};
        return String(reinterpret_cast<const char*>(Node->data.scalar.value), Node->data.scalar.length);
    }

    [[nodiscard]] auto ScalarView() const -> StringView {
        const auto* Node = Raw();
        if (!Node || Node->type != YAML_SCALAR_NODE)
            return {};
        return StringView(reinterpret_cast<const char*>(Node->data.scalar.value), Node->data.scalar.length);
    }

    [[nodiscard]] auto Size() const -> std::size_t {
        const auto* Node = Raw();
        if (!Node)
            return 0;
        if (Node->type == YAML_SEQUENCE_NODE)
            return static_cast<std::size_t>(Node->data.sequence.items.top - Node->data.sequence.items.start);
        if (Node->type == YAML_MAPPING_NODE)
            return static_cast<std::size_t>(Node->data.mapping.pairs.top - Node->data.mapping.pairs.start);
        return 0;
    }

    [[nodiscard]] auto size() const -> std::size_t {
        return Size();
    }

    [[nodiscard]] auto operator[](std::size_t Index) const -> YamlNode {
        const auto* Node = Raw();
        if (!Node || Node->type != YAML_SEQUENCE_NODE || Index >= Size())
            return {};
        return YamlNode{
            .Document = Document,
            .Id       = Node->data.sequence.items.start[Index],
        };
    }

    [[nodiscard]] auto MapEntries() const -> YamlMapRange;
};

struct YamlMapEntry {
    YamlNode First  = {};
    YamlNode Second = {};
};

[[nodiscard]] auto YamlMapIterator::operator*() const -> YamlMapEntry {
    return YamlMapEntry{
        .First  = YamlNode{.Document = Document, .Id = Pair->key},
        .Second = YamlNode{.Document = Document, .Id = Pair->value},
    };
}

[[nodiscard]] auto YamlNode::MapEntries() const -> YamlMapRange {
    const auto* Node = Raw();
    if (!Node || Node->type != YAML_MAPPING_NODE)
        return {};
    return YamlMapRange{
        .BeginIterator =
            YamlMapIterator{
                .Pair     = Node->data.mapping.pairs.start,
                .End      = Node->data.mapping.pairs.top,
                .Document = Document,
            },
        .EndIterator =
            YamlMapIterator{
                .Pair     = Node->data.mapping.pairs.top,
                .End      = Node->data.mapping.pairs.top,
                .Document = Document,
            },
    };
}

struct YamlParser {
    yaml_parser_t Value       = {};
    bool          Initialized = false;

    ~YamlParser() {
        if (Initialized)
            yaml_parser_delete(&Value);
    }

    YamlParser()                                     = default;
    YamlParser(const YamlParser&)                    = delete;
    auto operator=(const YamlParser&) -> YamlParser& = delete;
};

[[nodiscard]] auto MakeYamlParseError(StringView FilePath, const yaml_parser_t& Parser) -> ErrorMessage {
    const auto Problem = Parser.problem ? StringView(Parser.problem) : StringView("unknown YAML parser error");
    const auto Context = Parser.context ? StringView(Parser.context) : StringView();
    if (Context.empty()) {
        return ErrorMessage(Format("Failed to parse Scene document '{}': {} at line {}, column {}",
                                   FilePath,
                                   Problem,
                                   Parser.problem_mark.line + 1,
                                   Parser.problem_mark.column + 1));
    }
    return ErrorMessage(Format("Failed to parse Scene document '{}': {}: {} at line {}, column {}",
                               FilePath,
                               Context,
                               Problem,
                               Parser.problem_mark.line + 1,
                               Parser.problem_mark.column + 1));
}

[[nodiscard]] auto InitializeYamlParser(YamlParser& Parser, const String& Source, StringView FilePath)
    -> std::expected<void, ErrorMessage> {
    if (!yaml_parser_initialize(&Parser.Value))
        return std::unexpected(ErrorMessage(Format("Failed to initialize YAML parser for '{}'", FilePath)));
    Parser.Initialized = true;
    yaml_parser_set_input_string(&Parser.Value, reinterpret_cast<const unsigned char*>(Source.data()), Source.size());
    return {};
}

[[nodiscard]] auto ValidateYamlEventStream(const String& Source, StringView FilePath)
    -> std::expected<void, ErrorMessage> {
    YamlParser Parser = {};
    if (auto Result = InitializeYamlParser(Parser, Source, FilePath); !Result)
        return std::unexpected(Result.error());

    std::size_t  DocumentCount = 0;
    yaml_event_t Event         = {};
    while (true) {
        if (!yaml_parser_parse(&Parser.Value, &Event))
            return std::unexpected(MakeYamlParseError(FilePath, Parser.Value));

        const auto EventType          = Event.type;
        bool       InvalidFeature     = false;
        String     InvalidFeatureName = {};
        switch (EventType) {
        case YAML_DOCUMENT_START_EVENT:
            ++DocumentCount;
            if (Event.data.document_start.version_directive || Event.data.document_start.tag_directives.start) {
                InvalidFeature     = true;
                InvalidFeatureName = "YAML directives";
            }
            break;
        case YAML_ALIAS_EVENT:
            InvalidFeature     = true;
            InvalidFeatureName = "aliases";
            break;
        case YAML_SCALAR_EVENT:
            if (Event.data.scalar.anchor || Event.data.scalar.tag) {
                InvalidFeature     = true;
                InvalidFeatureName = Event.data.scalar.anchor ? "anchors" : "explicit tags";
            }
            break;
        case YAML_SEQUENCE_START_EVENT:
            if (Event.data.sequence_start.anchor || Event.data.sequence_start.tag) {
                InvalidFeature     = true;
                InvalidFeatureName = Event.data.sequence_start.anchor ? "anchors" : "explicit tags";
            }
            break;
        case YAML_MAPPING_START_EVENT:
            if (Event.data.mapping_start.anchor || Event.data.mapping_start.tag) {
                InvalidFeature     = true;
                InvalidFeatureName = Event.data.mapping_start.anchor ? "anchors" : "explicit tags";
            }
            break;
        default:
            break;
        }

        yaml_event_delete(&Event);
        if (InvalidFeature) {
            return std::unexpected(
                ErrorMessage(Format("Scene document at '{}': {} are not supported", FilePath, InvalidFeatureName)));
        }
        if (EventType == YAML_STREAM_END_EVENT)
            break;
    }
    if (DocumentCount != 1) {
        return std::unexpected(ErrorMessage(Format(
            "Scene document at '{}': must contain exactly one YAML document; found {}", FilePath, DocumentCount)));
    }
    return {};
}

[[nodiscard]] auto ParseYamlDocument(const String& Source, StringView FilePath)
    -> std::expected<YamlDocument, ErrorMessage> {
    if (auto Result = ValidateYamlEventStream(Source, FilePath); !Result)
        return std::unexpected(Result.error());

    YamlParser Parser = {};
    if (auto Result = InitializeYamlParser(Parser, Source, FilePath); !Result)
        return std::unexpected(Result.error());

    YamlDocument Document = {};
    if (!yaml_parser_load(&Parser.Value, &Document.Value))
        return std::unexpected(MakeYamlParseError(FilePath, Parser.Value));
    Document.Initialized = true;
    return Document;
}

[[nodiscard]] auto MakeStructuralError(StringView Path, StringView Message) -> std::unexpected<ErrorMessage> {
    return std::unexpected(ErrorMessage(Format("Scene document at '{}': {}", Path, Message)));
}

[[nodiscard]] auto MakeYamlPath(StringView Parent, StringView Child) -> String {
    if (Parent.empty())
        return String(Child);
    return Format("{}.{}", Parent, Child);
}

[[nodiscard]] auto ValidateYamlNode(const YamlNode& Node, StringView Path) -> std::expected<void, ErrorMessage> {
    if (Node.IsMap()) {
        std::unordered_set<String> Keys = {};
        for (const auto Entry : Node.MapEntries()) {
            if (!Entry.First.IsScalar())
                return MakeStructuralError(Path, "mapping key must be a scalar");
            const auto Key = Entry.First.Scalar();
            if (!Keys.insert(Key).second)
                return MakeStructuralError(MakeYamlPath(Path, Key), "contains a duplicate mapping key");
            if (auto Result = ValidateYamlNode(Entry.Second, MakeYamlPath(Path, Key)); !Result)
                return std::unexpected(Result.error());
        }
        return {};
    }
    if (Node.IsSequence()) {
        for (std::size_t Index = 0; Index < Node.size(); ++Index) {
            if (auto Result = ValidateYamlNode(Node[Index], Format("{}[{}]", Path, Index)); !Result)
                return std::unexpected(Result.error());
        }
    }
    return {};
}

[[nodiscard]] auto IsYamlDecimal(StringView Value) -> bool {
    if (Value.empty())
        return false;
    std::size_t Index = 0;
    if (Value[Index] == '+' || Value[Index] == '-')
        ++Index;
    const auto IntegerStart = Index;
    while (Index < Value.size() && Value[Index] >= '0' && Value[Index] <= '9')
        ++Index;
    const auto HasIntegerDigits  = Index != IntegerStart;
    auto       HasFractionDigits = false;
    if (Index < Value.size() && Value[Index] == '.') {
        ++Index;
        const auto FractionStart = Index;
        while (Index < Value.size() && Value[Index] >= '0' && Value[Index] <= '9')
            ++Index;
        HasFractionDigits = Index != FractionStart;
    }
    if (!HasIntegerDigits && !HasFractionDigits)
        return false;
    if (Index < Value.size() && (Value[Index] == 'e' || Value[Index] == 'E')) {
        ++Index;
        if (Index < Value.size() && (Value[Index] == '+' || Value[Index] == '-'))
            ++Index;
        const auto ExponentStart = Index;
        while (Index < Value.size() && Value[Index] >= '0' && Value[Index] <= '9')
            ++Index;
        if (Index == ExponentStart)
            return false;
    }
    return Index == Value.size();
}

[[nodiscard]] auto ReadYamlFloat(const YamlNode& Node, StringView Path, StringView Description)
    -> std::expected<float, ErrorMessage> {
    if (!Node.IsScalar())
        return MakeStructuralError(Path, Description);
    const auto Text = Node.ScalarView();
    if (!IsYamlDecimal(Text))
        return MakeStructuralError(Path, Format("could not decode {}", Description));
    float      Result       = {};
    const auto Start        = Text.starts_with('+') ? Text.data() + 1 : Text.data();
    const auto EndPtr       = Text.data() + Text.size();
    const auto [End, Error] = std::from_chars(Start, EndPtr, Result);
    if (Error != std::errc{} || End != EndPtr || !std::isfinite(Result))
        return MakeStructuralError(Path, Format("could not decode {}", Description));
    return Result;
}

[[nodiscard]] auto ReadYamlBool(const YamlNode& Node, StringView Path, StringView Description)
    -> std::expected<bool, ErrorMessage> {
    if (!Node.IsScalar())
        return MakeStructuralError(Path, Description);
    const auto Text = Node.ScalarView();
    if (Text == "true" || Text == "True" || Text == "TRUE")
        return true;
    if (Text == "false" || Text == "False" || Text == "FALSE")
        return false;
    return MakeStructuralError(Path, Format("could not decode {}", Description));
}

[[nodiscard]] auto ReadFloat3(const YamlNode& Node, StringView Path) -> std::expected<hlslpp::float3, ErrorMessage> {
    if (!Node.IsSequence() || Node.size() != 3)
        return MakeStructuralError(Path, "must be a sequence of exactly three numbers");

    std::array<float, 3> Components = {};
    for (std::size_t Index = 0; Index < 3; ++Index) {
        auto Value = ReadYamlFloat(Node[Index], MakeYamlPath(Path, Format("[{}]", Index)), "a number");
        if (!Value)
            return std::unexpected(Value.error());
        Components[Index] = Value.value();
    }
    return hlslpp::float3(Components[0], Components[1], Components[2]);
}

[[nodiscard]] auto ReadMaterialInstance(const YamlNode& Node, StringView Path)
    -> std::expected<PbrMaterial, ErrorMessage> {
    if (!Node.IsMap())
        return MakeStructuralError(Path, "must be a mapping");

    PbrMaterial Result = {};
    for (const auto Entry : Node.MapEntries()) {
        if (!Entry.First.IsScalar())
            return MakeStructuralError(Path, "contains a non-scalar key");
        const auto Key       = Entry.First.Scalar();
        const auto FieldPath = MakeYamlPath(Path, Key);
        if (Key == "base_color") {
            auto Value = ReadFloat3(Entry.Second, FieldPath);
            if (!Value)
                return std::unexpected(Value.error());
            Result.BaseColor = Value.value();
        } else if (Key == "metallic") {
            auto Value = ReadYamlFloat(Entry.Second, FieldPath, "a scalar number");
            if (!Value)
                return std::unexpected(Value.error());
            Result.Metallic = Value.value();
        } else if (Key == "roughness") {
            auto Value = ReadYamlFloat(Entry.Second, FieldPath, "a scalar number");
            if (!Value)
                return std::unexpected(Value.error());
            Result.Roughness = Value.value();
        } else if (Key == "emissive") {
            auto Value = ReadFloat3(Entry.Second, FieldPath);
            if (!Value)
                return std::unexpected(Value.error());
            Result.Emissive = Value.value();
        } else if (Key == "base_color_texture" || Key == "normal_texture" || Key == "metallic_roughness_texture" ||
                   Key == "metallic_texture" || Key == "roughness_texture" || Key == "occlusion_texture" ||
                   Key == "emissive_texture") {
            if (!Entry.Second.IsScalar())
                return MakeStructuralError(FieldPath, "a scalar string");
            const auto Texture = Entry.Second.Scalar();
            if (std::filesystem::path(Texture).is_absolute())
                return MakeStructuralError(FieldPath, "must be relative to the current application Assets directory");
            if (Key == "base_color_texture")
                Result.BaseColorTexture = Texture;
            else if (Key == "normal_texture")
                Result.NormalTexture = Texture;
            else if (Key == "metallic_roughness_texture")
                Result.MetallicRoughnessTexture = Texture;
            else if (Key == "metallic_texture")
                Result.MetallicTexture = Texture;
            else if (Key == "roughness_texture")
                Result.RoughnessTexture = Texture;
            else if (Key == "occlusion_texture")
                Result.OcclusionTexture = Texture;
            else
                Result.EmissiveTexture = Texture;
        } else {
            return MakeStructuralError(FieldPath, "is not a recognized PBR material field");
        }
    }

    if (!std::isfinite(Result.BaseColor.x) || !std::isfinite(Result.BaseColor.y) ||
        !std::isfinite(Result.BaseColor.z) || !std::isfinite(Result.Emissive.x) || !std::isfinite(Result.Emissive.y) ||
        !std::isfinite(Result.Emissive.z) || !std::isfinite(Result.Metallic) || !std::isfinite(Result.Roughness)) {
        return MakeStructuralError(Path, "contains a non-finite value");
    }
    if (Result.BaseColor.x < 0.0f || Result.BaseColor.x > 1.0f || Result.BaseColor.y < 0.0f ||
        Result.BaseColor.y > 1.0f || Result.BaseColor.z < 0.0f || Result.BaseColor.z > 1.0f) {
        return MakeStructuralError(MakeYamlPath(Path, "base_color"), "components must be in the range [0, 1]");
    }
    if (Result.Emissive.x < 0.0f || Result.Emissive.y < 0.0f || Result.Emissive.z < 0.0f)
        return MakeStructuralError(MakeYamlPath(Path, "emissive"), "components must be non-negative");
    if (Result.Metallic < 0.0f || Result.Metallic > 1.0f)
        return MakeStructuralError(MakeYamlPath(Path, "metallic"), "must be in the range [0, 1]");
    if (Result.Roughness < 0.0f || Result.Roughness > 1.0f)
        return MakeStructuralError(MakeYamlPath(Path, "roughness"), "must be in the range [0, 1]");
    return Result;
}

[[nodiscard]] auto LoadMaterialInstances(Scene& Scene, const YamlNode& Node, StringView Path)
    -> std::expected<void, ErrorMessage> {
    if (!Node.IsDefined() || Node.IsNull())
        return {};
    if (!Node.IsMap())
        return MakeStructuralError(Path, "must be a mapping");

    for (const auto Entry : Node.MapEntries()) {
        if (!Entry.First.IsScalar())
            return MakeStructuralError(Path, "material instance ID must be a scalar");
        const auto Id           = Entry.First.Scalar();
        const auto MaterialPath = MakeYamlPath(Path, Id);
        if (Id.empty())
            return MakeStructuralError(MaterialPath, "material instance ID must not be empty");
        auto Material = ReadMaterialInstance(Entry.Second, MaterialPath);
        if (!Material)
            return std::unexpected(Material.error());
        Scene.SetMaterialInstance(Id, Material.value());
    }
    return {};
}

[[nodiscard]] auto ReadTransform(const YamlNode& Node, StringView Path) -> std::expected<Transform, ErrorMessage> {
    Transform Result = {};
    if (!Node.IsDefined() || Node.IsNull())
        return Result;
    if (!Node.IsMap())
        return MakeStructuralError(Path, "must be a mapping");

    for (const auto Entry : Node.MapEntries()) {
        if (!Entry.First.IsScalar())
            return MakeStructuralError(Path, "contains a non-scalar key");
        const auto Key       = Entry.First.Scalar();
        const auto FieldPath = MakeYamlPath(Path, Key);
        if (Key == "translation") {
            auto Value = ReadFloat3(Entry.Second, FieldPath);
            if (!Value)
                return std::unexpected(Value.error());
            Result.Translation = Value.value();
        } else if (Key == "rotation") {
            auto Value = ReadFloat3(Entry.Second, FieldPath);
            if (!Value)
                return std::unexpected(Value.error());
            Result.Rotation = Value.value();
        } else if (Key == "scale") {
            auto Value = ReadFloat3(Entry.Second, FieldPath);
            if (!Value)
                return std::unexpected(Value.error());
            Result.Scale = Value.value();
        } else {
            return MakeStructuralError(FieldPath, "is not a recognized Transform field");
        }
    }
    return Result;
}

[[nodiscard]] auto
AssignYamlValue(const entt::meta_data& Field, entt::meta_any& Component, const YamlNode& Value, String& Error) -> bool {
    const auto Type = Field.type().info().hash();
    if (Type == entt::type_hash<float>::value()) {
        const auto ConvertedValue = ReadYamlFloat(Value, "", "a scalar number");
        if (!ConvertedValue) {
            Error = "must be a scalar number";
            return false;
        }
        if (!Field.set(Component, ConvertedValue.value())) {
            Error = "could not assign numeric value through metadata";
            return false;
        }
        return true;
    }
    if (Type == entt::type_hash<bool>::value()) {
        const auto ConvertedValue = ReadYamlBool(Value, "", "a scalar boolean");
        if (!ConvertedValue) {
            Error = "must be a scalar boolean";
            return false;
        }
        if (!Field.set(Component, ConvertedValue.value())) {
            Error = "could not assign boolean value through metadata";
            return false;
        }
        return true;
    }
    if (Type == entt::type_hash<String>::value()) {
        if (!Value.IsScalar()) {
            Error = "must be a scalar string";
            return false;
        }
        if (!Field.set(Component, Value.Scalar())) {
            Error = "could not assign string value through metadata";
            return false;
        }
        return true;
    }
    Error = "uses an unsupported persisted field type";
    return false;
}

auto AppendComponentWarning(SceneLoadReport& Report, String Path, String Message) -> void {
    Report.Warnings.emplace_back(ComponentWarning{
        .Path    = std::move(Path),
        .Message = std::move(Message),
    });
}

auto LoadComponents(Scene& Scene, entt::entity Entity, const YamlNode& Node, StringView Path, SceneLoadReport& Report)
    -> void {
    if (!Node.IsDefined() || Node.IsNull())
        return;
    if (!Node.IsMap()) {
        AppendComponentWarning(Report, String(Path), "components must be a mapping; all components were omitted");
        return;
    }

    auto& Registry = Scene.GetRegistry();
    for (const auto Entry : Node.MapEntries()) {
        if (!Entry.First.IsScalar()) {
            AppendComponentWarning(Report, String(Path), "component name must be a scalar; component was omitted");
            continue;
        }
        const auto ComponentName = Entry.First.Scalar();
        const auto ComponentPath = MakeYamlPath(Path, ComponentName);
        if (!Entry.Second.IsMap()) {
            AppendComponentWarning(Report, ComponentPath, "component data must be a mapping; component was omitted");
            continue;
        }

        const auto Type = entt::resolve(entt::hashed_string{ComponentName.c_str(), ComponentName.size()}.value());
        if (!Type) {
            AppendComponentWarning(Report, ComponentPath, "unknown component; component was omitted");
            continue;
        }
        entt::meta_any Component = {};
        if (Type == entt::resolve<CameraComponent>()) {
            Component = entt::forward_as_meta(Registry.emplace<CameraComponent>(Entity));
        } else if (Type == entt::resolve<MeshComponent>()) {
            Component = entt::forward_as_meta(Registry.emplace<MeshComponent>(Entity));
        } else if (Type == entt::resolve<LightComponent>()) {
            Component = entt::forward_as_meta(Registry.emplace<LightComponent>(Entity));
        } else {
            AppendComponentWarning(Report, ComponentPath, "component is not supported by the Scene document loader");
            continue;
        }
        bool Valid = true;
        for (const auto FieldEntry : Entry.Second.MapEntries()) {
            if (!FieldEntry.First.IsScalar()) {
                AppendComponentWarning(Report, ComponentPath, "component field name must be a scalar");
                Valid = false;
                break;
            }
            const auto FieldName = FieldEntry.First.Scalar();
            const auto FieldPath = MakeYamlPath(ComponentPath, FieldName);
            const auto Field     = Type.data(entt::hashed_string{FieldName.c_str(), FieldName.size()}.value());
            if (!Field) {
                AppendComponentWarning(Report, FieldPath, "unknown component field");
                Valid = false;
                break;
            }
            String Error;
            if (!AssignYamlValue(Field, Component, FieldEntry.Second, Error)) {
                AppendComponentWarning(Report, FieldPath, std::move(Error));
                Valid = false;
                break;
            }
        }

        if (!Valid) {
            if (Type == entt::resolve<CameraComponent>())
                Registry.remove<CameraComponent>(Entity);
            else if (Type == entt::resolve<MeshComponent>())
                Registry.remove<MeshComponent>(Entity);
            else
                Registry.remove<LightComponent>(Entity);
        }
    }
}

[[nodiscard]] auto
LoadEntity(Scene& Scene, const YamlNode& Node, entt::entity Parent, StringView Path, SceneLoadReport& Report)
    -> std::expected<void, ErrorMessage> {
    if (!Node.IsMap())
        return MakeStructuralError(Path, "entity must be a mapping");

    String    Name           = {};
    Transform LocalTransform = {};
    YamlNode  Components     = {};
    YamlNode  Children       = {};
    for (const auto Entry : Node.MapEntries()) {
        if (!Entry.First.IsScalar())
            return MakeStructuralError(Path, "entity contains a non-scalar key");
        const auto Key       = Entry.First.Scalar();
        const auto FieldPath = MakeYamlPath(Path, Key);
        if (Key == "name") {
            if (!Entry.Second.IsScalar())
                return MakeStructuralError(FieldPath, "must be a scalar string");
            Name = Entry.Second.Scalar();
        } else if (Key == "transform") {
            auto TransformResult = ReadTransform(Entry.Second, FieldPath);
            if (!TransformResult)
                return std::unexpected(TransformResult.error());
            LocalTransform = TransformResult.value();
        } else if (Key == "components") {
            Components = Entry.Second;
        } else if (Key == "children") {
            Children = Entry.Second;
        } else {
            return MakeStructuralError(FieldPath, "is not a recognized entity field");
        }
    }

    const auto Entity                                         = Scene.CreateEntity(std::move(Name), Parent);
    Scene.GetRegistry().get<SceneNode>(Entity).LocalTransform = LocalTransform;
    LoadComponents(Scene, Entity, Components, MakeYamlPath(Path, "components"), Report);

    if (!Children.IsDefined() || Children.IsNull())
        return {};
    if (!Children.IsSequence())
        return MakeStructuralError(MakeYamlPath(Path, "children"), "must be a sequence");
    for (std::size_t Index = 0; Index < Children.size(); ++Index) {
        if (auto Result = LoadEntity(Scene, Children[Index], Entity, Format("{}.children[{}]", Path, Index), Report);
            !Result)
            return std::unexpected(Result.error());
    }
    return {};
}

} // namespace

[[nodiscard]] auto Scene::LoadFromFile(const Path& FilePath) -> std::expected<SceneLoadReport, ErrorMessage> {
    const auto Source = ReadFile(FilePath);
    if (!Source)
        return std::unexpected(Source.error().Append(Format("Failed to load Scene document '{}'", FilePath.string())));
    auto Document = ParseYamlDocument(Source.value(), FilePath.string());
    if (!Document)
        return std::unexpected(Document.error());

    const auto* RootRaw = yaml_document_get_root_node(&Document->Value);
    const auto  Root    = YamlNode{
        .Document = &Document->Value,
        .Id       = RootRaw ? static_cast<int>(RootRaw - Document->Value.nodes.start) + 1 : 0,
    };
    if (!Root.IsMap())
        return MakeStructuralError(FilePath.string(), "document root must be a mapping");
    if (auto Result = ValidateYamlNode(Root, FilePath.string()); !Result)
        return std::unexpected(Result.error());

    YamlNode MaterialInstances = {};
    YamlNode Entities          = {};
    for (const auto Entry : Root.MapEntries()) {
        if (!Entry.First.IsScalar())
            return MakeStructuralError(FilePath.string(), "document contains a non-scalar key");
        const auto Key = Entry.First.Scalar();
        if (Key == "material_instances") {
            MaterialInstances = Entry.Second;
        } else if (Key == "entities") {
            Entities = Entry.Second;
        } else {
            return MakeStructuralError(MakeYamlPath(FilePath.string(), Key), "is not a recognized Scene File field");
        }
    }
    if (!Entities || !Entities.IsSequence())
        return MakeStructuralError(MakeYamlPath(FilePath.string(), "entities"), "must be a sequence");

    Scene Temporary       = {};
    Temporary.m_AssetRoot = (FilePath.parent_path() / "Assets").lexically_normal();
    if (auto Result = LoadMaterialInstances(Temporary, MaterialInstances, "material_instances"); !Result)
        return std::unexpected(Result.error().Append(Format("Failed to load Scene document '{}'", FilePath.string())));

    SceneLoadReport Report = {};
    for (std::size_t Index = 0; Index < Entities.size(); ++Index) {
        if (auto Result = LoadEntity(Temporary, Entities[Index], entt::null, Format("entities[{}]", Index), Report);
            !Result)
            return std::unexpected(
                Result.error().Append(Format("Failed to load Scene document '{}'", FilePath.string())));
    }
    Temporary.UpdateWorldTransforms();

    // Publish the scene's material instances to the engine-wide manager. Texture paths
    // are resolved against the scene Assets root here, so the render thread always reads
    // canonical absolute paths. Scene replacement clears the whole table first, so a
    // new scene never inherits stale instances. Both happen only after the whole
    // document validated successfully, so a failed load leaves the previous manager
    // state (and the previous Scene) untouched.
    MaterialManager::Get().Clear();
    for (const auto& [Id, Material] : Temporary.m_MaterialInstances) {
        auto       Copy               = Material;
        const auto ResolveTexturePath = [&](String& Texture) -> void {
            if (!Texture.empty())
                Texture = Temporary.ResolveAssetPath(Texture);
        };
        ResolveTexturePath(Copy.BaseColorTexture);
        ResolveTexturePath(Copy.NormalTexture);
        ResolveTexturePath(Copy.MetallicRoughnessTexture);
        ResolveTexturePath(Copy.MetallicTexture);
        ResolveTexturePath(Copy.RoughnessTexture);
        ResolveTexturePath(Copy.OcclusionTexture);
        ResolveTexturePath(Copy.EmissiveTexture);
        MaterialManager::Get().RegisterMaterial(String(Id), Copy);
    }

    // Register all mesh assets with GeometryManager
    auto& GeometryMgr = GeometryManager::Get();
    GeometryMgr.Clear();
    
    // Collect unique mesh asset paths
    std::unordered_set<String> MeshAssets;
    const auto Meshes = Temporary.m_Registry->view<MeshComponent>();
    for (const auto Entity : Meshes) {
        const auto& Mesh = Meshes.get<MeshComponent>(Entity);
        if (!Mesh.Asset.empty()) {
            MeshAssets.insert(Temporary.ResolveAssetPath(Mesh.Asset));
        }
    }
    
    // Load and register each unique mesh
    for (const auto& MeshPath : MeshAssets) {
        Assimp::Importer Importer;
        constexpr Uint32 Flags = aiProcess_Triangulate | aiProcess_FlipUVs | aiProcess_CalcTangentSpace |
                                 aiProcess_GenSmoothNormals | aiProcess_JoinIdenticalVertices |
                                 aiProcess_ImproveCacheLocality | aiProcess_OptimizeMeshes;

        const auto* Scene = Importer.ReadFile(MeshPath.c_str(), Flags);
        if (!Scene || !Scene->mRootNode || Scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) {
            LogWarning("Failed to load mesh '{}': {}", MeshPath, Importer.GetErrorString());
            continue;
        }

        const Path MeshDirectory = Path(MeshPath).parent_path();
        
        // Register materials from the mesh
        MaterialManager::Get().RegisterMaterialsFromScene(Scene, MeshDirectory);
        
        // Register geometry
        GeometryMgr.RegisterScene(Scene, MeshDirectory, MeshPath);
    }

    *this = std::move(Temporary);
    return Report;
}

} // namespace SoulEngine