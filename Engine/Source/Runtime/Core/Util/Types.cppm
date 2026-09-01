module;

#include <entt/core/type_traits.hpp>
#include <entt/meta/type_traits.hpp>
#include <hlsl++.h>

export module Core:Util.Types;

export import std;

// hlslpp comparison operators follow HLSL semantics: vector == vector yields a
// component-wise vector result rather than bool. That return type is a hard
// error inside EnTT's is_equality_comparable probe (not SFINAE fallout), so any
// entt::meta_any holding an hlslpp vector type fails to compile. Declare these
// types as not equality comparable for EnTT; the meta_any vtable then skips
// comparison wiring for them. Exported here so every module unit importing Core
// shares this single definition.
export namespace entt {

template <>
struct is_equality_comparable<hlslpp::float2> : std::false_type {};

template <>
struct is_equality_comparable<hlslpp::float3> : std::false_type {};

template <>
struct is_equality_comparable<hlslpp::float4> : std::false_type {};

// std::filesystem::path satisfies EnTT's meta_sequence_container_like concept (it has
// value_type and begin/end) but lacks const_reference, which breaks the sequence
// container facade. Declaring (never defining) this explicit specialization keeps
// Path out of the container category: is_complete_v stays false for it.
template <>
struct meta_sequence_container_traits<std::filesystem::path>;

} // namespace entt

export namespace SoulEngine {

using Int8  = std::int8_t;
using Int16 = std::int16_t;
using Int32 = std::int32_t;
using Int64 = std::int64_t;

using Uint8  = std::uint8_t;
using Uint16 = std::uint16_t;
using Uint32 = std::uint32_t;
using Uint64 = std::uint64_t;

// Clang (≤ 22.1.6) does not yet support <stdfloat> from C++23.
using Float32 = float;  // TODO: replace with std::float32_t when Clang supports it
using Float64 = double; // TODO: replace with std::float64_t when Clang supports it

using String     = std::string;
using StringView = std::string_view;

using Path = std::filesystem::path;

// Format wrapper avoids ambiguous std::format when both import std; (modules)
// and #include-based standard library headers (e.g. Vulkan/GLFW/VMA) are present.
template <typename... Args>
[[nodiscard]] auto Format(std::format_string<Args...> Fmt, Args&&... InArgs) -> String {
    return std::format(Fmt, std::forward<Args>(InArgs)...);
}

template <typename T>
using UPtr = std::unique_ptr<T>;

template <typename T>
using SPtr = std::shared_ptr<T>;

} // namespace SoulEngine
