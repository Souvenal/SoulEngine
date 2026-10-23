module;

#include <hlsl++.h>
#include <yaml.h>

module Core.Util.Yaml;

import std;

namespace SoulEngine {

[[nodiscard]] auto YamlNode::IsNull() const -> bool {
    if (!IsScalar())
        return false;
    const auto Value = Scalar();
    return Value.empty() || Value == "~" || Value == "null" || Value == "Null" || Value == "NULL";
}

[[nodiscard]] auto YamlNode::IsScalar() const -> bool {
    return std::holds_alternative<String>(m_Value);
}

[[nodiscard]] auto YamlNode::IsSequence() const -> bool {
    return std::holds_alternative<Sequence>(m_Value);
}

[[nodiscard]] auto YamlNode::IsMap() const -> bool {
    return std::holds_alternative<Mapping>(m_Value);
}

[[nodiscard]] auto YamlNode::Scalar() const -> String {
    const auto* Value = std::get_if<String>(&m_Value);
    if (!Value)
        return {};
    return *Value;
}

[[nodiscard]] auto YamlNode::Location() const -> String {
    return Format("line {}, column {}", m_Line, m_Column);
}

[[nodiscard]] auto YamlNode::Size() const -> std::size_t {
    if (const auto* SequenceValue = std::get_if<Sequence>(&m_Value))
        return SequenceValue->size();
    if (const auto* MappingValue = std::get_if<Mapping>(&m_Value))
        return MappingValue->size();
    return 0;
}

[[nodiscard]] auto YamlNode::size() const -> std::size_t {
    return Size();
}

[[nodiscard]] auto YamlNode::operator[](StringView Key) const -> std::optional<std::reference_wrapper<const YamlNode>> {
    if (!IsMap())
        return std::nullopt;

    for (const auto& [EntryKey, EntryValue] : std::get<Mapping>(m_Value)) {
        if (EntryKey == Key)
            return std::optional<std::reference_wrapper<const YamlNode>>{*EntryValue};
    }
    return std::nullopt;
}

[[nodiscard]] auto YamlNode::Values() const -> std::vector<std::reference_wrapper<const YamlNode>> {
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

[[nodiscard]] auto YamlNode::Items() const -> const Mapping& {
    const auto* MappingValue = std::get_if<Mapping>(&m_Value);
    if (!MappingValue) {
        static const Mapping EmptyMapping = {};
        return EmptyMapping;
    }
    return *MappingValue;
}

YamlNode::YamlNode(String Value) : m_Value(std::move(Value)) {}

YamlNode::YamlNode(Mapping Value) : m_Value(std::move(Value)) {}

YamlNode::YamlNode(Sequence Value) : m_Value(std::move(Value)) {}

auto YamlDocument::operator=(YamlDocument&& Other) noexcept -> YamlDocument& {
    if (this == &Other)
        return *this;
    std::swap(m_Root, Other.m_Root);
    return *this;
}

[[nodiscard]] auto YamlDocument::Root() const -> const YamlNode& {
    return *m_Root;
}

[[nodiscard]] auto AssignYamlField(const FieldDescriptor& Field, void* Object, const YamlNode& Value)
    -> std::expected<void, ErrorMessage> {
    std::optional<FieldValue> Converted   = {};
    String                    Expectation = {};
    switch (Field.Type) {
    case FieldType::Float32:
        Expectation = "must be a scalar number";
        if (const auto V = Value.As<float>())
            Converted = FieldValue{*V};
        break;
    case FieldType::Bool:
        Expectation = "must be a scalar boolean";
        if (const auto V = Value.As<bool>())
            Converted = FieldValue{*V};
        break;
    case FieldType::String:
        Expectation = "must be a scalar string";
        if (const auto V = Value.As<String>())
            Converted = FieldValue{*V};
        break;
    case FieldType::Path:
        Expectation = "must be a scalar string";
        if (const auto V = Value.As<String>())
            Converted = FieldValue{Path{*V}};
        break;
    case FieldType::Float3:
        Expectation = "must be a sequence of exactly three numbers";
        if (const auto V = Value.As<hlslpp::float3>())
            Converted = FieldValue{*V};
        break;
    case FieldType::Float4:
        Expectation = "must be a sequence of exactly four numbers";
        if (const auto V = Value.As<hlslpp::float4>())
            Converted = FieldValue{*V};
        break;
    }
    if (!Converted)
        return std::unexpected(ErrorMessage(Expectation));
    if (!Field.Set(Object, *Converted))
        return std::unexpected(ErrorMessage("could not assign value through the field descriptor"));
    return {};
}

} // namespace SoulEngine
