/// @file   Reflection.cppm
/// @brief  Descriptor-based reflection kernel: a closed-variant field value type plus per-type field
///         tables, shared by YAML-driven loading and editor property panels.
module;

#include <hlsl++.h>

export module Core.Util.Reflection;

export import Core.Util.Types;

import std;

namespace SoulEngine {

/// @brief Wire-level vocabulary of reflected field types.
export enum class FieldType : Uint8 {
    Float32, ///< Scalar number.
    Bool,    ///< Scalar boolean.
    String,  ///< Scalar string; also carries enum values bridged through setter/getter pairs.
    Path,    ///< Scalar string decoded as a filesystem path.
    Float3,  ///< Sequence of exactly three numbers.
    Float4,  ///< Sequence of exactly four numbers.
};

/// @brief One reflected field value; the active alternative matches the descriptor's FieldType.
export using FieldValue = std::variant<Float32, bool, String, Path, hlslpp::float3, hlslpp::float4>;

/// @brief Accessor pair for one named field of a reflected type.
export struct FieldDescriptor {
    StringView Name = {};                 ///< Wire name used by serialized documents and panels.
    FieldType  Type = FieldType::Float32; ///< Active FieldValue alternative for this field.
    auto (*Get)(const void* Owner) -> FieldValue              = nullptr;
    auto (*Set)(void* Owner, const FieldValue& Value) -> bool = nullptr;
};

/// @brief Field descriptors of one reflected type.
export using FieldTable = std::span<const FieldDescriptor>;

/// @brief Return the descriptor named Name, or nullptr when the table has no such field.
export [[nodiscard]] constexpr auto FindField(FieldTable Fields, StringView Name) -> const FieldDescriptor* {
    for (const auto& Descriptor : Fields) {
        if (Descriptor.Name == Name)
            return &Descriptor;
    }
    return nullptr;
}

namespace Detail {

template <typename T>
inline constexpr auto FieldTypeOf = [] {
    if constexpr (std::same_as<T, Float32>)
        return FieldType::Float32;
    else if constexpr (std::same_as<T, bool>)
        return FieldType::Bool;
    else if constexpr (std::same_as<T, String>)
        return FieldType::String;
    else if constexpr (std::same_as<T, Path>)
        return FieldType::Path;
    else if constexpr (std::same_as<T, hlslpp::float3>)
        return FieldType::Float3;
    else if constexpr (std::same_as<T, hlslpp::float4>)
        return FieldType::Float4;
    else
        static_assert(!sizeof(T*), "unsupported reflected field type");
}();

template <typename>
struct MemberPointerTraits;

template <typename OwnerT, typename MemberT>
struct MemberPointerTraits<MemberT OwnerT::*> {
    using Owner  = OwnerT;
    using Member = MemberT;
};

} // namespace Detail

/// @brief Build the descriptor of a data member; accessors derive from the member pointer.
export template <auto MemberPtr>
    requires std::is_member_object_pointer_v<decltype(MemberPtr)>
[[nodiscard]] consteval auto Field(StringView Name) -> FieldDescriptor {
    using Traits = Detail::MemberPointerTraits<decltype(MemberPtr)>;
    using Owner  = typename Traits::Owner;
    using Member = typename Traits::Member;
    return FieldDescriptor{
        .Name = Name,
        .Type = Detail::FieldTypeOf<Member>,
        .Get  = [](const void* Object) -> FieldValue {
            return FieldValue{static_cast<const Owner*>(Object)->*MemberPtr};
        },
        .Set = [](void* Object, const FieldValue& Value) -> bool {
            const auto* Typed = std::get_if<Member>(&Value);
            if (!Typed)
                return false;
            static_cast<Owner*>(Object)->*MemberPtr = *Typed;
            return true;
        },
    };
}

/// @brief Build the descriptor of a field exposed through a String setter/getter pair.
export template <auto Setter, auto Getter>
[[nodiscard]] consteval auto Field(StringView Name) -> FieldDescriptor {
    using Owner = typename Detail::MemberPointerTraits<decltype(Getter)>::Owner;
    static_assert(std::same_as<std::invoke_result_t<decltype(Getter), const Owner&>, String>);
    static_assert(std::same_as<std::invoke_result_t<decltype(Setter), Owner&, String>, void>);
    return FieldDescriptor{
        .Name = Name,
        .Type = FieldType::String,
        .Get  = [](const void* Object) -> FieldValue {
            return FieldValue{std::invoke(Getter, *static_cast<const Owner*>(Object))};
        },
        .Set = [](void* Object, const FieldValue& Value) -> bool {
            const auto* Typed = std::get_if<String>(&Value);
            if (!Typed)
                return false;
            std::invoke(Setter, *static_cast<Owner*>(Object), *Typed);
            return true;
        },
    };
}

} // namespace SoulEngine
