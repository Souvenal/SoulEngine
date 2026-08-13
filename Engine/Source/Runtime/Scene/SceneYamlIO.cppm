module;

#include <entt/entt.hpp>
#include <hlsl++.h>
#include <yaml.h>

module Scene:YamlIO;

import Scene;

namespace SoulEngine {

// TRY macro: unwraps std::expected, propagates error on failure.
// Usage: TRY(name, expr) — assigns unwrapped value to `name`, or returns error.
#define TRY(name, expr)                                                                                                \
    auto _try_##name = (expr);                                                                                         \
    if (!_try_##name)                                                                                                  \
        return std::unexpected(_try_##name.error());                                                                   \
    auto name = _try_##name.value()

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

struct YamlEmitter {
    yaml_emitter_t Value       = {};
    bool           Initialized = false;

    ~YamlEmitter() {
        if (Initialized)
            yaml_emitter_delete(&Value);
    }

    YamlEmitter()                                      = default;
    YamlEmitter(const YamlEmitter&)                    = delete;
    auto operator=(const YamlEmitter&) -> YamlEmitter& = delete;
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

[[nodiscard]] auto MakeYamlEmitterError(StringView FilePath, const yaml_emitter_t& Emitter) -> ErrorMessage {
    const auto Problem = Emitter.problem ? StringView(Emitter.problem) : StringView("unknown YAML emitter error");
    return ErrorMessage(Format("Failed to emit Scene document '{}': {}", FilePath, Problem));
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

struct YamlBuilder {
    YamlDocument Document = {};

    [[nodiscard]] auto AddScalar(StringView Value, yaml_scalar_style_t Style) -> std::expected<int, ErrorMessage> {
        if (Value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
            return std::unexpected(ErrorMessage("YAML scalar is too large to emit"));
        const auto Id = yaml_document_add_scalar(&Document.Value,
                                                 reinterpret_cast<const yaml_char_t*>(YAML_STR_TAG),
                                                 reinterpret_cast<const yaml_char_t*>(Value.data()),
                                                 static_cast<int>(Value.size()),
                                                 Style);
        if (Id == 0)
            return std::unexpected(ErrorMessage("YAML document could not allocate a scalar node"));
        return Id;
    }

    [[nodiscard]] auto AddSequence(yaml_sequence_style_t Style) -> std::expected<int, ErrorMessage> {
        const auto Id =
            yaml_document_add_sequence(&Document.Value, reinterpret_cast<const yaml_char_t*>(YAML_SEQ_TAG), Style);
        if (Id == 0)
            return std::unexpected(ErrorMessage("YAML document could not allocate a sequence node"));
        return Id;
    }

    [[nodiscard]] auto AddMapping(yaml_mapping_style_t Style) -> std::expected<int, ErrorMessage> {
        const auto Id =
            yaml_document_add_mapping(&Document.Value, reinterpret_cast<const yaml_char_t*>(YAML_MAP_TAG), Style);
        if (Id == 0)
            return std::unexpected(ErrorMessage("YAML document could not allocate a mapping node"));
        return Id;
    }

    [[nodiscard]] auto AppendSequence(int Sequence, int Item) -> std::expected<void, ErrorMessage> {
        if (!yaml_document_append_sequence_item(&Document.Value, Sequence, Item))
            return std::unexpected(ErrorMessage("YAML document could not append a sequence item"));
        return {};
    }

    [[nodiscard]] auto AppendMapping(int Mapping, int Key, int Value) -> std::expected<void, ErrorMessage> {
        if (!yaml_document_append_mapping_pair(&Document.Value, Mapping, Key, Value))
            return std::unexpected(ErrorMessage("YAML document could not append a mapping pair"));
        return {};
    }
};

struct SceneFieldSchema {
    StringView    Name = {};
    entt::id_type Id   = {};
};

struct SceneComponentSchema {
    entt::meta_any (*Create)(entt::registry&, SceneEntity);
    void (*Remove)(entt::registry&, SceneEntity);
    [[nodiscard]] auto (*Has)(const entt::registry&, SceneEntity) -> bool;
    [[nodiscard]] entt::meta_any (*Get)(entt::registry&, SceneEntity);
    [[nodiscard]] auto (*Validate)(const Scene&, const entt::meta_any&, String&) -> bool;
    StringView                        Name   = {};
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

[[nodiscard]] auto ValidateCamera(const Scene&, const entt::meta_any& Value, String& Error) -> bool {
    const auto* Camera = Value.try_cast<CameraComponent>();
    if (!Camera) {
        Error = "Camera component metadata does not contain CameraComponent";
        return false;
    }
    if (Camera->Settings.FOV <= 0.0f || Camera->Settings.FOV >= 179.0f) {
        Error = "fov_degrees must be in the range (0, 179)";
        return false;
    }
    if (Camera->Settings.NearPlane <= 0.0f) {
        Error = "near_plane must be greater than zero";
        return false;
    }
    if (Camera->Settings.FarPlane <= Camera->Settings.NearPlane) {
        Error = "far_plane must be greater than near_plane";
        return false;
    }
    if (!std::isfinite(Camera->Settings.ExposureEV100)) {
        Error = "exposure_ev100 must be finite";
        return false;
    }
    return true;
}

[[nodiscard]] auto ValidateMesh(const Scene& Scene, const entt::meta_any& Value, String& Error) -> bool {
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
    if (!Mesh->Material.empty() && !Scene.FindMaterialInstance(Mesh->Material)) {
        Error = Format("material instance '{}' does not exist", Mesh->Material);
        return false;
    }
    return true;
}

[[nodiscard]] auto ValidateLight(const Scene&, const entt::meta_any& Value, String& Error) -> bool {
    const auto* Light = Value.try_cast<LightComponent>();
    if (!Light) {
        Error = "Light component metadata does not contain LightComponent";
        return false;
    }
    if (Light->Type == LightType::Unknown) {
        Error = "type must be directional, point, or spot";
        return false;
    }
    const auto Luminance = 0.2126f * Light->ColorR + 0.7152f * Light->ColorG + 0.0722f * Light->ColorB;
    if (!std::isfinite(Light->ColorR) || !std::isfinite(Light->ColorG) || !std::isfinite(Light->ColorB) ||
        !std::isfinite(Light->Intensity) || !std::isfinite(Light->RangeMeters) ||
        !std::isfinite(Light->InnerConeAngleDegrees) || !std::isfinite(Light->OuterConeAngleDegrees)) {
        Error = "contains a non-finite value";
        return false;
    }
    if (Light->ColorR < 0.0f || Light->ColorG < 0.0f || Light->ColorB < 0.0f || std::abs(Luminance - 1.0f) > 0.001f) {
        Error = "color must be non-negative linear sRGB with Rec.709 luminance equal to one";
        return false;
    }
    if (Light->Intensity < 0.0f) {
        Error = "intensity must be non-negative";
        return false;
    }
    if (Light->Type != LightType::Directional && Light->RangeMeters <= 0.0f) {
        Error = "range_meters must be greater than zero for point and spot lights";
        return false;
    }
    if (Light->Type == LightType::Spot &&
        (Light->InnerConeAngleDegrees <= 0.0f || Light->InnerConeAngleDegrees > Light->OuterConeAngleDegrees ||
         Light->OuterConeAngleDegrees >= 90.0f)) {
        Error = "spot cone angles must satisfy 0 < inner <= outer < 90 degrees";
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
        SceneFieldSchema{.Name = "exposure_ev100", .Id = entt::hashed_string{"exposure_ev100"}.value()},
    };
    static const std::array MeshFields{
        SceneFieldSchema{.Name = "asset", .Id = entt::hashed_string{"asset"}.value()},
        SceneFieldSchema{.Name = "material", .Id = entt::hashed_string{"material"}.value()},
    };

    static const std::array LightFields{
        SceneFieldSchema{.Name = "type", .Id = entt::hashed_string{"type"}.value()},
        SceneFieldSchema{.Name = "color_r", .Id = entt::hashed_string{"color_r"}.value()},
        SceneFieldSchema{.Name = "color_g", .Id = entt::hashed_string{"color_g"}.value()},
        SceneFieldSchema{.Name = "color_b", .Id = entt::hashed_string{"color_b"}.value()},
        SceneFieldSchema{.Name = "intensity", .Id = entt::hashed_string{"intensity"}.value()},
        SceneFieldSchema{.Name = "range_meters", .Id = entt::hashed_string{"range_meters"}.value()},
        SceneFieldSchema{.Name = "inner_cone_angle_degrees",
                         .Id   = entt::hashed_string{"inner_cone_angle_degrees"}.value()},
        SceneFieldSchema{.Name = "outer_cone_angle_degrees",
                         .Id   = entt::hashed_string{"outer_cone_angle_degrees"}.value()},
        SceneFieldSchema{.Name = "casts_shadows", .Id = entt::hashed_string{"casts_shadows"}.value()},
    };
    entt::meta_factory<CameraComponent>{}
        .type(entt::hashed_string{"camera"}.value())
        .custom<SceneComponentSchema>(SceneComponentSchema{
            .Create   = &CreateComponent<CameraComponent>,
            .Remove   = &RemoveComponent<CameraComponent>,
            .Has      = &HasComponent<CameraComponent>,
            .Get      = &GetComponent<CameraComponent>,
            .Validate = &ValidateCamera,
            .Name     = "camera",
            .Fields   = CameraFields,
        })
        .data<&CameraComponent::SetFOV, &CameraComponent::GetFOV>(CameraFields[0].Id)
        .data<&CameraComponent::SetNearPlane, &CameraComponent::GetNearPlane>(CameraFields[1].Id)
        .data<&CameraComponent::SetFarPlane, &CameraComponent::GetFarPlane>(CameraFields[2].Id)
        .data<&CameraComponent::SetExposureEV100, &CameraComponent::GetExposureEV100>(CameraFields[3].Id);

    entt::meta_factory<MeshComponent>{}
        .type(entt::hashed_string{"mesh"}.value())
        .custom<SceneComponentSchema>(SceneComponentSchema{
            .Create   = &CreateComponent<MeshComponent>,
            .Remove   = &RemoveComponent<MeshComponent>,
            .Has      = &HasComponent<MeshComponent>,
            .Get      = &GetComponent<MeshComponent>,
            .Validate = &ValidateMesh,
            .Name     = "mesh",
            .Fields   = MeshFields,
        })
        .data<&MeshComponent::Asset>(MeshFields[0].Id)
        .data<&MeshComponent::Material>(MeshFields[1].Id);

    entt::meta_factory<LightComponent>{}
        .type(entt::hashed_string{"light"}.value())
        .custom<SceneComponentSchema>(SceneComponentSchema{
            .Create   = &CreateComponent<LightComponent>,
            .Remove   = &RemoveComponent<LightComponent>,
            .Has      = &HasComponent<LightComponent>,
            .Get      = &GetComponent<LightComponent>,
            .Validate = &ValidateLight,
            .Name     = "light",
            .Fields   = LightFields,
        })
        .data<&LightComponent::SetType, &LightComponent::GetType>(LightFields[0].Id)
        .data<&LightComponent::ColorR>(LightFields[1].Id)
        .data<&LightComponent::ColorG>(LightFields[2].Id)
        .data<&LightComponent::ColorB>(LightFields[3].Id)
        .data<&LightComponent::Intensity>(LightFields[4].Id)
        .data<&LightComponent::RangeMeters>(LightFields[5].Id)
        .data<&LightComponent::InnerConeAngleDegrees>(LightFields[6].Id)
        .data<&LightComponent::OuterConeAngleDegrees>(LightFields[7].Id)
        .data<&LightComponent::CastsShadows>(LightFields[8].Id);
    Registered = true;
}
[[nodiscard]] auto ValidateSingleCamera(const Scene& Scene, StringView Path) -> std::expected<void, ErrorMessage> {
    std::size_t CameraCount = 0;
    for (const auto Entity : Scene.GetRegistry().view<CameraComponent>()) {
        static_cast<void>(Entity);
        ++CameraCount;
    }
    if (CameraCount != 1)
        return MakeStructuralError(Path, Format("must contain exactly one camera component; found {}", CameraCount));
    return {};
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
    -> std::expected<PbrMetallicRoughnessMaterial, ErrorMessage> {
    if (!Node.IsMap())
        return MakeStructuralError(Path, "must be a mapping");

    PbrMetallicRoughnessMaterial Result = {};
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

auto LoadComponents(Scene& Scene, SceneEntity Entity, const YamlNode& Node, StringView Path, SceneLoadReport& Report)
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

        const auto  Type   = entt::resolve(entt::hashed_string{ComponentName.c_str(), ComponentName.size()}.value());
        const auto* Schema = static_cast<SceneComponentSchema*>(Type.custom());
        if (!Type || !Schema) {
            AppendComponentWarning(Report, ComponentPath, "unknown component; component was omitted");
            continue;
        }

        auto Component = Schema->Create(Registry, Entity);
        bool Valid     = true;
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

        String Error;
        if (Valid && !Schema->Validate(Scene, Component, Error)) {
            AppendComponentWarning(Report, ComponentPath, std::move(Error));
            Valid = false;
        }
        if (!Valid)
            Schema->Remove(Registry, Entity);
    }
}

[[nodiscard]] auto
LoadEntity(Scene& Scene, const YamlNode& Node, SceneEntity Parent, StringView Path, SceneLoadReport& Report)
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

    const auto Entity                                    = Scene.CreateEntity(std::move(Name), Parent);
    Scene.GetRegistry().get<SceneNode>(Entity).Transform = LocalTransform;
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

[[nodiscard]] auto FormatYamlFloat(float Value) -> std::expected<String, ErrorMessage> {
    if (!std::isfinite(Value))
        return std::unexpected(ErrorMessage("Scene document cannot emit a non-finite floating-point value"));
    std::array<char, 64> Buffer = {};
    const auto [End, Error]     = std::to_chars(Buffer.data(), Buffer.data() + Buffer.size(), Value);
    if (Error != std::errc{})
        return std::unexpected(ErrorMessage("Scene document could not format a floating-point value"));
    return String(Buffer.data(), End);
}

[[nodiscard]] auto AddYamlFloat(YamlBuilder& Builder, float Value) -> std::expected<int, ErrorMessage> {
    const auto Text = FormatYamlFloat(Value);
    if (!Text)
        return std::unexpected(Text.error());
    return Builder.AddScalar(Text.value(), YAML_PLAIN_SCALAR_STYLE);
}

[[nodiscard]] auto SaveFloat3(YamlBuilder& Builder, const hlslpp::float3& Value) -> std::expected<int, ErrorMessage> {
    const auto Result = Builder.AddSequence(YAML_FLOW_SEQUENCE_STYLE);
    if (!Result)
        return std::unexpected(Result.error());
    for (const auto Element : {static_cast<float>(Value.x), static_cast<float>(Value.y), static_cast<float>(Value.z)}) {
        const auto SavedElement = AddYamlFloat(Builder, Element);
        if (!SavedElement)
            return std::unexpected(SavedElement.error());
        if (auto Appended = Builder.AppendSequence(Result.value(), SavedElement.value()); !Appended)
            return std::unexpected(Appended.error());
    }
    return Result;
}

[[nodiscard]] auto SaveYamlValue(YamlBuilder& Builder, const entt::meta_data& Field, const entt::meta_any& Component)
    -> std::expected<int, ErrorMessage> {
    const auto Value = Field.get(Component);
    if (!Value)
        return std::unexpected(ErrorMessage("Scene component metadata could not read a persisted field"));
    if (const auto* Float = Value.try_cast<float>())
        return AddYamlFloat(Builder, *Float);
    if (const auto* Bool = Value.try_cast<bool>())
        return Builder.AddScalar(*Bool ? "true" : "false", YAML_PLAIN_SCALAR_STYLE);
    if (const auto* StringValue = Value.try_cast<String>())
        return Builder.AddScalar(*StringValue, YAML_DOUBLE_QUOTED_SCALAR_STYLE);
    return std::unexpected(ErrorMessage("Scene component metadata contains an unsupported persisted field type"));
}

[[nodiscard]] auto AddYamlMappingEntry(YamlBuilder& Builder, int Mapping, StringView Key, int Value)
    -> std::expected<void, ErrorMessage> {
    const auto SavedKey = Builder.AddScalar(Key, YAML_PLAIN_SCALAR_STYLE);
    if (!SavedKey)
        return std::unexpected(SavedKey.error());
    return Builder.AppendMapping(Mapping, SavedKey.value(), Value);
}

[[nodiscard]] auto SaveEntity(YamlBuilder& Builder, const Scene& Scene, SceneEntity Entity)
    -> std::expected<int, ErrorMessage> {
    const auto& Registry = Scene.GetRegistry();
    const auto& Node     = Registry.get<SceneNode>(Entity);
    TRY(EntityNode, Builder.AddMapping(YAML_BLOCK_MAPPING_STYLE));

    if (!Node.Name.empty()) {
        TRY(SavedName, Builder.AddScalar(Node.Name, YAML_DOUBLE_QUOTED_SCALAR_STYLE));
        if (auto R = AddYamlMappingEntry(Builder, EntityNode, "name", SavedName); !R)
            return std::unexpected(R.error());
    }

    TRY(TransformNode, Builder.AddMapping(YAML_BLOCK_MAPPING_STYLE));
    TRY(Translation, SaveFloat3(Builder, Node.Transform.Translation));
    if (auto R = AddYamlMappingEntry(Builder, TransformNode, "translation", Translation); !R)
        return std::unexpected(R.error());
    TRY(Rotation, SaveFloat3(Builder, Node.Transform.Rotation));
    if (auto R = AddYamlMappingEntry(Builder, TransformNode, "rotation", Rotation); !R)
        return std::unexpected(R.error());
    TRY(Scale, SaveFloat3(Builder, Node.Transform.Scale));
    if (auto R = AddYamlMappingEntry(Builder, TransformNode, "scale", Scale); !R)
        return std::unexpected(R.error());
    if (auto R = AddYamlMappingEntry(Builder, EntityNode, "transform", TransformNode); !R)
        return std::unexpected(R.error());

    TRY(Components, Builder.AddMapping(YAML_BLOCK_MAPPING_STYLE));
    bool HasComponents = false;
    for (const auto& [UnusedId, Type] : entt::resolve()) {
        static_cast<void>(UnusedId);
        const auto* Schema = static_cast<SceneComponentSchema*>(Type.custom());
        if (!Schema || !Schema->Has(Registry, Entity))
            continue;

        TRY(ComponentNode, Builder.AddMapping(YAML_BLOCK_MAPPING_STYLE));
        const auto Component = Schema->Get(const_cast<entt::registry&>(Registry), Entity);
        for (const auto& FieldSchema : Schema->Fields) {
            const auto Field = Type.data(FieldSchema.Id);
            auto       Value = SaveYamlValue(Builder, Field, Component);
            if (!Value)
                return std::unexpected(Value.error().Append(Format("Failed to save component '{}'", Schema->Name)));
            if (auto Appended = AddYamlMappingEntry(Builder, ComponentNode, FieldSchema.Name, Value.value()); !Appended)
                return std::unexpected(Appended.error().Append(Format("Failed to save component '{}'", Schema->Name)));
        }
        if (auto Appended = AddYamlMappingEntry(Builder, Components, Schema->Name, ComponentNode); !Appended)
            return std::unexpected(Appended.error().Append(Format("Failed to save component '{}'", Schema->Name)));
        HasComponents = true;
    }
    if (HasComponents) {
        if (auto R = AddYamlMappingEntry(Builder, EntityNode, "components", Components); !R)
            return std::unexpected(R.error());
    }

    if (!Node.Children.empty()) {
        TRY(Children, Builder.AddSequence(YAML_BLOCK_SEQUENCE_STYLE));
        for (const auto Child : Node.Children) {
            TRY(SavedChild, SaveEntity(Builder, Scene, Child));
            if (auto R = Builder.AppendSequence(Children, SavedChild); !R)
                return std::unexpected(R.error());
        }
        if (auto R = AddYamlMappingEntry(Builder, EntityNode, "children", Children); !R)
            return std::unexpected(R.error());
    }
    return EntityNode;
}

[[nodiscard]] auto SaveMaterialInstance(YamlBuilder& Builder, const PbrMetallicRoughnessMaterial& Material)
    -> std::expected<int, ErrorMessage> {
    const auto MaterialNode = Builder.AddMapping(YAML_BLOCK_MAPPING_STYLE);
    if (!MaterialNode)
        return std::unexpected(MaterialNode.error());

    const auto BaseColor = SaveFloat3(Builder, Material.BaseColor);
    if (!BaseColor)
        return std::unexpected(BaseColor.error());
    if (auto Result = AddYamlMappingEntry(Builder, MaterialNode.value(), "base_color", BaseColor.value()); !Result)
        return std::unexpected(Result.error());

    const auto Metallic = AddYamlFloat(Builder, Material.Metallic);
    if (!Metallic)
        return std::unexpected(Metallic.error());
    if (auto Result = AddYamlMappingEntry(Builder, MaterialNode.value(), "metallic", Metallic.value()); !Result)
        return std::unexpected(Result.error());

    const auto Roughness = AddYamlFloat(Builder, Material.Roughness);
    if (!Roughness)
        return std::unexpected(Roughness.error());
    if (auto Result = AddYamlMappingEntry(Builder, MaterialNode.value(), "roughness", Roughness.value()); !Result)
        return std::unexpected(Result.error());

    const auto Emissive = SaveFloat3(Builder, Material.Emissive);
    if (!Emissive)
        return std::unexpected(Emissive.error());
    if (auto Result = AddYamlMappingEntry(Builder, MaterialNode.value(), "emissive", Emissive.value()); !Result)
        return std::unexpected(Result.error());

    const auto AddTexture = [&](StringView Name, const String& Texture) -> std::expected<void, ErrorMessage> {
        if (Texture.empty())
            return {};
        const auto SavedTexture = Builder.AddScalar(Texture, YAML_DOUBLE_QUOTED_SCALAR_STYLE);
        if (!SavedTexture)
            return std::unexpected(SavedTexture.error());
        return AddYamlMappingEntry(Builder, MaterialNode.value(), Name, SavedTexture.value());
    };
    for (const auto& [Name, Texture] : std::array{
             std::pair<StringView, const String&>{"base_color_texture", Material.BaseColorTexture},
             std::pair<StringView, const String&>{"normal_texture", Material.NormalTexture},
             std::pair<StringView, const String&>{"metallic_roughness_texture", Material.MetallicRoughnessTexture},
             std::pair<StringView, const String&>{"metallic_texture", Material.MetallicTexture},
             std::pair<StringView, const String&>{"roughness_texture", Material.RoughnessTexture},
             std::pair<StringView, const String&>{"occlusion_texture", Material.OcclusionTexture},
             std::pair<StringView, const String&>{"emissive_texture", Material.EmissiveTexture},
         }) {
        if (auto Result = AddTexture(Name, Texture); !Result)
            return std::unexpected(Result.error());
    }
    return MaterialNode;
}

} // namespace

[[nodiscard]] auto Scene::LoadFromFile(const Path& FilePath) -> std::expected<SceneLoadReport, ErrorMessage> {
    RegisterBuiltInComponentSchemas();

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
    if (auto Result = ValidateSingleCamera(Temporary, FilePath.string()); !Result)
        return std::unexpected(Result.error().Append(Format("Failed to load Scene document '{}'", FilePath.string())));
    Temporary.UpdateWorldTransforms();
    *this = std::move(Temporary);
    return Report;
}

[[nodiscard]] auto Scene::SaveToFile(const Path& FilePath) const -> std::expected<void, ErrorMessage> {
    RegisterBuiltInComponentSchemas();

    const auto FilePathString = FilePath.string();
    const auto Ctx            = [&FilePathString](auto&& R) {
        return R.error().Append(Format("Failed to save Scene document '{}'", FilePathString));
    };

    if (auto Result = ValidateSingleCamera(*this, FilePathString); !Result)
        return std::unexpected(Ctx(Result));

    YamlBuilder Builder = {};
    if (!yaml_document_initialize(&Builder.Document.Value, nullptr, nullptr, nullptr, 1, 1))
        return std::unexpected(ErrorMessage(Format("Failed to initialize YAML document for '{}'", FilePathString)));
    Builder.Document.Initialized = true;

    TRY(Document, Builder.AddMapping(YAML_BLOCK_MAPPING_STYLE));

    if (!m_MaterialInstances.empty()) {
        TRY(MaterialInstances, Builder.AddMapping(YAML_BLOCK_MAPPING_STYLE));
        for (const auto& [Id, Material] : GetMaterialInstances()) {
            TRY(MaterialNode, SaveMaterialInstance(Builder, Material));
            if (auto Result = AddYamlMappingEntry(Builder, MaterialInstances, Id, MaterialNode); !Result)
                return std::unexpected(Ctx(Result));
        }
        if (auto Result = AddYamlMappingEntry(Builder, Document, "material_instances", MaterialInstances); !Result)
            return std::unexpected(Ctx(Result));
    }

    TRY(Entities, Builder.AddSequence(YAML_BLOCK_SEQUENCE_STYLE));
    for (const auto Root : m_Roots) {
        TRY(SavedRoot, SaveEntity(Builder, *this, Root));
        if (auto Result = Builder.AppendSequence(Entities, SavedRoot); !Result)
            return std::unexpected(Ctx(Result));
    }
    if (auto Result = AddYamlMappingEntry(Builder, Document, "entities", Entities); !Result)
        return std::unexpected(Ctx(Result));

    auto* Output = std::fopen(FilePathString.c_str(), "wb");
    if (!Output)
        return std::unexpected(ErrorMessage(Format("Failed to open Scene document '{}' for writing", FilePathString)));

    YamlEmitter Emitter = {};
    if (!yaml_emitter_initialize(&Emitter.Value)) {
        std::fclose(Output);
        return std::unexpected(ErrorMessage(Format("Failed to initialize YAML emitter for '{}'", FilePathString)));
    }
    Emitter.Initialized = true;
    yaml_emitter_set_output_file(&Emitter.Value, Output);
    yaml_emitter_set_indent(&Emitter.Value, 2);
    yaml_emitter_set_width(&Emitter.Value, -1);

    if (!yaml_emitter_open(&Emitter.Value)) {
        std::fclose(Output);
        return std::unexpected(MakeYamlEmitterError(FilePathString, Emitter.Value));
    }

    auto       DocumentToEmit  = std::move(Builder.Document);
    const auto Dumped          = yaml_emitter_dump(&Emitter.Value, &DocumentToEmit.Value);
    // yaml_emitter_dump() takes and destroys the document even when emission fails.
    DocumentToEmit.Initialized = false;
    if (!Dumped) {
        std::fclose(Output);
        return std::unexpected(MakeYamlEmitterError(FilePathString, Emitter.Value));
    }

    if (!yaml_emitter_close(&Emitter.Value)) {
        std::fclose(Output);
        return std::unexpected(MakeYamlEmitterError(FilePathString, Emitter.Value));
    }
    if (std::fclose(Output) != 0)
        return std::unexpected(ErrorMessage(Format("Failed to write Scene document '{}'", FilePathString)));
    return {};
}

} // namespace SoulEngine
