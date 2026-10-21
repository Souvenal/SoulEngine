module;

#include <entt/entt.hpp>
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

[[nodiscard]] auto AssignYamlValue(const entt::meta_data& Field, entt::meta_any& Component, const YamlNode& Value)
    -> std::expected<void, ErrorMessage> {
    // Compare runtime type indices, not type_hash values: EnTT's type_hash derives from __PRETTY_FUNCTION__,
    // whose spelling of std types (e.g. "std::string" vs "std::basic_string<char>") varies between module TUs
    // that see the declarations through different include/import paths, and link order decides which spelling
    // wins. The runtime type_index is one process-wide counter slot per type and is stable across TUs.
    const auto Type = Field.type().info().index();
    if (Type == entt::type_id<float>().index()) {
        const auto ConvertedValue = Value.As<float>();
        if (!ConvertedValue)
            return std::unexpected(ErrorMessage("must be a scalar number"));
        if (!Field.set(Component, ConvertedValue.value()))
            return std::unexpected(ErrorMessage("could not assign numeric value through metadata"));
        return {};
    }
    if (Type == entt::type_id<bool>().index()) {
        const auto ConvertedValue = Value.As<bool>();
        if (!ConvertedValue)
            return std::unexpected(ErrorMessage("must be a scalar boolean"));
        if (!Field.set(Component, ConvertedValue.value()))
            return std::unexpected(ErrorMessage("could not assign boolean value through metadata"));
        return {};
    }
    if (Type == entt::type_id<String>().index()) {
        const auto ConvertedValue = Value.As<String>();
        if (!ConvertedValue)
            return std::unexpected(ErrorMessage("must be a scalar string"));
        if (!Field.set(Component, ConvertedValue.value()))
            return std::unexpected(ErrorMessage("could not assign string value through metadata"));
        return {};
    }
    if (Type == entt::type_id<hlslpp::float3>().index()) {
        const auto ConvertedValue = Value.As<hlslpp::float3>();
        if (!ConvertedValue)
            return std::unexpected(ErrorMessage("must be a sequence of exactly three numbers"));
        if (!Field.set(Component, ConvertedValue.value()))
            return std::unexpected(ErrorMessage("could not assign float3 value through metadata"));
        return {};
    }
    if (Type == entt::type_id<Path>().index()) {
        const auto ConvertedValue = Value.As<String>();
        if (!ConvertedValue)
            return std::unexpected(ErrorMessage("must be a scalar string"));
        if (!Field.set(Component, Path{ConvertedValue.value()}))
            return std::unexpected(ErrorMessage("could not assign path value through metadata"));
        return {};
    }
    if (Type == entt::type_id<hlslpp::float4>().index()) {
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
