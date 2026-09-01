/// @file   Yaml.cppm
/// @brief  libyaml-backed YAML document reader: validated-subset parsing into an owned node tree,
///         plus EnTT meta field assignment shared by YAML-driven record loading.

module;

#include <entt/entt.hpp>
#include <hlsl++.h>
#include <yaml.h>

export module Core:Util.Yaml;

export import :Util.Types;
export import :Util.Error;
export import :Util.IO;
export import std;

namespace SoulEngine {

namespace {

class YamlParser {
  public:
    [[nodiscard]] static auto Create(std::span<const unsigned char> Source)
        -> std::expected<YamlParser, ErrorMessage> {
        YamlParser Result = {};
        if (!yaml_parser_initialize(&Result.m_Parser))
            return std::unexpected(ErrorMessage("Failed to initialize YAML parser"));
        Result.m_Initialized = true;
        Result.m_Source      = Source;
        return Result;
    }

    [[nodiscard]] auto LoadToDocument() -> std::expected<yaml_document_t, ErrorMessage> {
        if (auto Result = Validate(); !Result)
            return std::unexpected(Result.error());

        // The validation pass consumed the input stream and libyaml does not support
        // rewinding or re-setting the input on a used parser, so reset it first.
        yaml_parser_delete(&m_Parser);
        if (!yaml_parser_initialize(&m_Parser)) {
            m_Initialized = false;
            return std::unexpected(ErrorMessage("Failed to re-initialize YAML parser"));
        }

        yaml_parser_set_input_string(&m_Parser, m_Source.data(), m_Source.size());
        yaml_document_t Document = {};
        if (!yaml_parser_load(&m_Parser, &Document))
            return std::unexpected(ErrorMessage("Failed to load YAML document"));
        return Document;
    }

    ~YamlParser() {
        if (m_Initialized)
            yaml_parser_delete(&m_Parser);
    }

    YamlParser(const YamlParser&)                    = delete;
    auto operator=(const YamlParser&) -> YamlParser& = delete;

    YamlParser(YamlParser&& Other) noexcept
        : m_Parser(std::exchange(Other.m_Parser, {})),
          m_Source(std::exchange(Other.m_Source, {})),
          m_Initialized(std::exchange(Other.m_Initialized, false)) {}

    auto operator=(YamlParser&& Other) noexcept -> YamlParser& {
        if (this == &Other)
            return *this;
        std::swap(m_Parser, Other.m_Parser);
        std::swap(m_Source, Other.m_Source);
        std::swap(m_Initialized, Other.m_Initialized);
        return *this;
    }

  private:
    YamlParser() = default;

    [[nodiscard]] auto Validate() -> std::expected<void, ErrorMessage> {
        yaml_parser_set_input_string(&m_Parser, m_Source.data(), m_Source.size());

        std::size_t  DocumentCount = 0;
        yaml_event_t Event         = {};
        while (true) {
            if (!yaml_parser_parse(&m_Parser, &Event)) {
                const auto Problem = m_Parser.problem ? StringView(m_Parser.problem)
                                                      : StringView("unknown YAML parser error");
                const auto Context = m_Parser.context ? StringView(m_Parser.context) : StringView();
                if (Context.empty()) {
                    return std::unexpected(ErrorMessage(Format(
                        "{} at line {}, column {}",
                        Problem,
                        m_Parser.problem_mark.line + 1,
                        m_Parser.problem_mark.column + 1)));
                }
                return std::unexpected(ErrorMessage(Format(
                    "{}: {} at line {}, column {}",
                    Context,
                    Problem,
                    m_Parser.problem_mark.line + 1,
                    m_Parser.problem_mark.column + 1)));
            }

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
            if (InvalidFeature)
                return std::unexpected(
                    ErrorMessage(Format("{} are not supported", InvalidFeatureName)));
            if (EventType == YAML_STREAM_END_EVENT)
                break;
        }
        if (DocumentCount != 1)
            return std::unexpected(
                ErrorMessage(Format("must contain exactly one YAML document; found {}", DocumentCount)));
        return {};
    }

    std::span<const unsigned char> m_Source      = {};
    yaml_parser_t m_Parser      = {};
    bool          m_Initialized = false;
};

} // namespace

} // namespace SoulEngine

export namespace SoulEngine {

/// @brief One node of a parsed YAML document tree.
///
/// Heap-allocated through Create() only; scalar, sequence, and mapping values are
/// distinguished via IsScalar()/IsSequence()/IsMap() and read through As<T>(),
/// Values(), and Items().
class YamlNode {
    friend class YamlDocument;

  public:
    using Mapping  = std::vector<std::pair<String, UPtr<YamlNode>>>;
    using Sequence = std::vector<UPtr<YamlNode>>;

  private:
    [[nodiscard]] static auto Create(yaml_document_t* Document, int Id)
        -> std::expected<UPtr<YamlNode>, ErrorMessage> {
        const auto* Node = Document && Id != 0 ? yaml_document_get_node(Document, Id) : nullptr;
        if (!Node)
            return std::unexpected(ErrorMessage("YAML node does not exist"));

        // Attach the 1-based source location so diagnostics can point into the document.
        const auto WithLocation = [&Node](UPtr<YamlNode> Result) -> UPtr<YamlNode> {
            Result->m_Line   = static_cast<Uint32>(Node->start_mark.line + 1);
            Result->m_Column = static_cast<Uint32>(Node->start_mark.column + 1);
            return Result;
        };

        if (Node->type == YAML_SCALAR_NODE) {
            return WithLocation(std::make_unique<YamlNode>(
                YamlNode{String(reinterpret_cast<const char*>(Node->data.scalar.value), Node->data.scalar.length)}));
        }

        if (Node->type == YAML_SEQUENCE_NODE) {
            Sequence Result = {};
            Result.reserve(static_cast<std::size_t>(Node->data.sequence.items.top -
                                                    Node->data.sequence.items.start));
            for (auto* Item = Node->data.sequence.items.start; Item != Node->data.sequence.items.top; ++Item) {
                auto Child = Create(Document, *Item);
                if (!Child)
                    return std::unexpected(Child.error());
                Result.emplace_back(std::move(Child.value()));
            }
            return WithLocation(std::make_unique<YamlNode>(YamlNode{std::move(Result)}));
        }

        if (Node->type == YAML_MAPPING_NODE) {
            Mapping Result = {};
            Result.reserve(static_cast<std::size_t>(Node->data.mapping.pairs.top -
                                                    Node->data.mapping.pairs.start));
            std::unordered_set<String> Keys = {};
            for (auto* Pair = Node->data.mapping.pairs.start; Pair != Node->data.mapping.pairs.top; ++Pair) {
                auto Key = Create(Document, Pair->key);
                if (!Key)
                    return std::unexpected(Key.error());
                if (!(*Key)->IsScalar())
                    return std::unexpected(ErrorMessage("YAML mapping keys must be scalar values"));

                auto KeyString = (*Key)->Scalar();
                if (!Keys.insert(KeyString).second) {
                    const auto* KeyNode = yaml_document_get_node(Document, Pair->key);
                    return std::unexpected(ErrorMessage(Format(
                        "YAML mapping contains a duplicate key '{}' at line {}, column {}",
                        KeyString,
                        KeyNode->start_mark.line + 1,
                        KeyNode->start_mark.column + 1)));
                }

                auto Value = Create(Document, Pair->value);
                if (!Value)
                    return std::unexpected(Value.error());
                Result.emplace_back(std::move(KeyString), std::move(Value.value()));
            }
            return WithLocation(std::make_unique<YamlNode>(YamlNode{std::move(Result)}));
        }

        return std::unexpected(ErrorMessage("YAML node has an unsupported type"));
    }

  public:
    [[nodiscard]] auto IsNull() const -> bool {
        if (!IsScalar())
            return false;
        const auto Value = Scalar();
        return Value.empty() || Value == "~" || Value == "null" || Value == "Null" || Value == "NULL";
    }

    [[nodiscard]] auto IsScalar() const -> bool {
        return std::holds_alternative<String>(m_Value);
    }

    [[nodiscard]] auto IsSequence() const -> bool {
        return std::holds_alternative<Sequence>(m_Value);
    }

    [[nodiscard]] auto IsMap() const -> bool {
        return std::holds_alternative<Mapping>(m_Value);
    }

    [[nodiscard]] auto Scalar() const -> String {
        const auto* Value = std::get_if<String>(&m_Value);
        if (!Value)
            return {};
        return *Value;
    }

    /// @brief 1-based source location of this node within the originating document,
    /// formatted as "line L, column C" for diagnostics.
    [[nodiscard]] auto Location() const -> String {
        return Format("line {}, column {}", m_Line, m_Column);
    }

    /// @brief Decode the node as T. Scalar types require a scalar node; hlslpp vector
    /// types require a sequence of exactly that many numbers. Returns nullopt on mismatch.
    template <typename T>
    [[nodiscard]] auto As() const -> std::optional<T> {
        if constexpr (std::same_as<T, hlslpp::float3> || std::same_as<T, hlslpp::float4>) {
            constexpr std::size_t ComponentCount = std::same_as<T, hlslpp::float3> ? 3 : 4;
            const auto*     SequenceValue      = std::get_if<Sequence>(&m_Value);
            if (!SequenceValue || SequenceValue->size() != ComponentCount)
                return std::nullopt;
            float Components[ComponentCount] = {};
            for (std::size_t Index = 0; Index < ComponentCount; ++Index) {
                const auto Component = (*SequenceValue)[Index]->As<float>();
                if (!Component)
                    return std::nullopt;
                Components[Index] = *Component;
            }
            if constexpr (std::same_as<T, hlslpp::float3>)
                return hlslpp::float3(Components[0], Components[1], Components[2]);
            else
                return hlslpp::float4(Components[0], Components[1], Components[2], Components[3]);
        } else {
            const auto* ScalarValue = std::get_if<String>(&m_Value);
            if (!ScalarValue)
                return std::nullopt;

            if constexpr (std::same_as<T, String>) {
                return *ScalarValue;
            } else if constexpr (std::same_as<T, float>) {
                const StringView Text   = *ScalarValue;
                float            Result = {};
                const auto       Start  = Text.starts_with('+') ? Text.data() + 1 : Text.data();
                const auto       EndPtr = Text.data() + Text.size();
                const auto [End, Error] = std::from_chars(Start, EndPtr, Result, std::chars_format::general);
                if (Error != std::errc{} || End != EndPtr || !std::isfinite(Result))
                    return std::nullopt;
                return Result;
            } else if constexpr (std::same_as<T, bool>) {
                const StringView Text = *ScalarValue;
                if (Text == "true" || Text == "True" || Text == "TRUE")
                    return true;
                if (Text == "false" || Text == "False" || Text == "FALSE")
                    return false;
                return std::nullopt;
            } else {
                static_assert(!sizeof(T), "unsupported YamlNode value type");
            }
        }
    }

    [[nodiscard]] auto Size() const -> std::size_t {
        if (const auto* SequenceValue = std::get_if<Sequence>(&m_Value))
            return SequenceValue->size();
        if (const auto* MappingValue = std::get_if<Mapping>(&m_Value))
            return MappingValue->size();
        return 0;
    }

    [[nodiscard]] auto size() const -> std::size_t {
        return Size();
    }

    /// @brief Look up a mapping entry by key. Returns nullopt for non-mappings and missing keys.
    [[nodiscard]] auto operator[](StringView Key) const
        -> std::optional<std::reference_wrapper<const YamlNode>> {
        if (!IsMap())
            return std::nullopt;

        for (const auto& [EntryKey, EntryValue] : std::get<Mapping>(m_Value)) {
            if (EntryKey == Key)
                return std::optional<std::reference_wrapper<const YamlNode>>{*EntryValue};
        }
        return std::nullopt;
    }

    /// @brief Child nodes of a mapping (values only) or sequence, in document order.
    [[nodiscard]] auto Values() const -> std::vector<std::reference_wrapper<const YamlNode>> {
        std::vector<std::reference_wrapper<const YamlNode>> Result = {};
        if (const auto* MappingValue = std::get_if<Mapping>(&m_Value)) {
            Result.reserve(MappingValue->size());
            for (const auto& [Key, Value] : *MappingValue)
                Result.emplace_back(*Value);
            return Result;
        }
        if (const auto* SequenceValue = std::get_if<Sequence>(&m_Value)) {
            Result.reserve(SequenceValue->size());
            for (const auto& Value : *SequenceValue)
                Result.emplace_back(*Value);
        }
        return Result;
    }

    /// @brief Key/value pairs of a mapping in document order; empty for non-mappings.
    [[nodiscard]] auto Items() const -> const Mapping& {
        const auto* MappingValue = std::get_if<Mapping>(&m_Value);
        if (!MappingValue) {
            static const Mapping EmptyMapping = {};
            return EmptyMapping;
        }
        return *MappingValue;
    }

  private:
    explicit YamlNode(String Value) : m_Value(std::move(Value)) {}
    explicit YamlNode(Mapping Value) : m_Value(std::move(Value)) {}
    explicit YamlNode(Sequence Value) : m_Value(std::move(Value)) {}

    Uint32                                  m_Line   = 0;
    Uint32                                  m_Column = 0;
    std::variant<String, Mapping, Sequence> m_Value;
};

/// @brief An owned, validated YAML document tree loaded from a file.
///
/// Parsing accepts a single-document subset: no anchors, aliases, explicit tags, or
/// directives, and duplicate mapping keys are rejected with line/column information.
class YamlDocument {
  public:
    [[nodiscard]] static auto Create(const Path& FilePath) -> std::expected<YamlDocument, ErrorMessage> {
        const auto Source = ReadFile(FilePath);
        if (!Source)
            return std::unexpected(Source.error().Append(Format("Error when reading '{}'", FilePath.string())));

        const auto Bytes = std::span<const unsigned char>(
            reinterpret_cast<const unsigned char*>(Source->data()), Source->size());
        auto Parser = YamlParser::Create(Bytes);
        if (!Parser)
            return std::unexpected(Parser.error().Append(Format("Error when reading '{}'", FilePath.string())));

        auto ParsedDocument = Parser->LoadToDocument();
        if (!ParsedDocument)
            return std::unexpected(
                ParsedDocument.error().Append(Format("Error when reading '{}'", FilePath.string())));

        YamlDocument Document = {};
        const auto* Root = yaml_document_get_root_node(&ParsedDocument.value());
        if (!Root)
            return std::unexpected(ErrorMessage("YAML document is empty"));
        const auto RootId = static_cast<int>(Root - ParsedDocument->nodes.start) + 1;
        auto RootNode = YamlNode::Create(&ParsedDocument.value(), RootId);
        yaml_document_delete(&ParsedDocument.value());
        if (!RootNode)
            return std::unexpected(RootNode.error().Append(Format("Error when reading '{}'", FilePath.string())));
        Document.m_Root = std::move(RootNode.value());
        return Document;
    }

    YamlDocument()                                     = default;
    YamlDocument(const YamlDocument&)                  = delete;
    auto operator=(const YamlDocument&) -> YamlDocument& = delete;

    YamlDocument(YamlDocument&& Other) noexcept
        : m_Root(std::exchange(Other.m_Root, {})) {}

    auto operator=(YamlDocument&& Other) noexcept -> YamlDocument& {
        if (this == &Other)
            return *this;
        std::swap(m_Root, Other.m_Root);
        return *this;
    }

    [[nodiscard]] auto Root() const -> const YamlNode& {
        return *m_Root;
    }

  private:
    UPtr<YamlNode>  m_Root        = {};
};

/// @brief Decode a YAML node as the registered type of a meta field and assign it.
///
/// The YAML side is untyped; the target type comes from the field's registered meta
/// type. Fails when the node does not decode as that type or the meta assignment fails.
[[nodiscard]] inline auto
AssignYamlValue(const entt::meta_data& Field, entt::meta_any& Component, const YamlNode& Value)
    -> std::expected<void, ErrorMessage> {
    const auto Type = Field.type().info().hash();
    if (Type == entt::type_hash<float>::value()) {
        const auto ConvertedValue = Value.As<float>();
        if (!ConvertedValue)
            return std::unexpected(ErrorMessage("must be a scalar number"));
        if (!Field.set(Component, ConvertedValue.value()))
            return std::unexpected(ErrorMessage("could not assign numeric value through metadata"));
        return {};
    }
    if (Type == entt::type_hash<bool>::value()) {
        const auto ConvertedValue = Value.As<bool>();
        if (!ConvertedValue)
            return std::unexpected(ErrorMessage("must be a scalar boolean"));
        if (!Field.set(Component, ConvertedValue.value()))
            return std::unexpected(ErrorMessage("could not assign boolean value through metadata"));
        return {};
    }
    if (Type == entt::type_hash<String>::value()) {
        const auto ConvertedValue = Value.As<String>();
        if (!ConvertedValue)
            return std::unexpected(ErrorMessage("must be a scalar string"));
        if (!Field.set(Component, ConvertedValue.value()))
            return std::unexpected(ErrorMessage("could not assign string value through metadata"));
        return {};
    }
    if (Type == entt::type_hash<hlslpp::float3>::value()) {
        const auto ConvertedValue = Value.As<hlslpp::float3>();
        if (!ConvertedValue)
            return std::unexpected(ErrorMessage("must be a sequence of exactly three numbers"));
        if (!Field.set(Component, ConvertedValue.value()))
            return std::unexpected(ErrorMessage("could not assign float3 value through metadata"));
        return {};
    }
    if (Type == entt::type_hash<Path>::value()) {
        const auto ConvertedValue = Value.As<String>();
        if (!ConvertedValue)
            return std::unexpected(ErrorMessage("must be a scalar string"));
        if (!Field.set(Component, Path{ConvertedValue.value()}))
            return std::unexpected(ErrorMessage("could not assign path value through metadata"));
        return {};
    }
    if (Type == entt::type_hash<hlslpp::float4>::value()) {
        const auto ConvertedValue = Value.As<hlslpp::float4>();
        if (!ConvertedValue)
            return std::unexpected(ErrorMessage("must be a sequence of exactly four numbers"));
        if (!Field.set(Component, ConvertedValue.value()))
            return std::unexpected(ErrorMessage("could not assign float4 value through metadata"));
        return {};
    }
    return std::unexpected(ErrorMessage("uses an unsupported persisted field type"));
}

} // namespace SoulEngine
