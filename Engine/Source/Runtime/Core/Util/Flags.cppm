export module Core:Util.Flags;

import magic_enum;

import std;

export namespace SoulEngine {

/// @brief Marker concept for the engine's flag enums: an enum qualifies once it specializes
/// `magic_enum::customize::enum_range<E>` with `is_flags = true`.
///
/// That specialization already switches on magic_enum's flags reflection (`enum_flags_name`, and
/// `enum_cast` of `"A|B"` strings); this concept reads it a second time as the opt-in for flag composition
/// below, so one declaration marks an enum as flags for both.
template <typename E>
concept FlagEnum = std::is_enum_v<E> && requires { magic_enum::customize::enum_range<E>::is_flags; } &&
                   magic_enum::customize::enum_range<E>::is_flags;

/// @brief Compose two flag values of one enum into their union.
///
/// The FlagEnum constraint keeps this operator out of the overload set for every other enum — Vulkan's
/// `vk::` flag bits among them — so backend code composing native flags from inside this namespace still
/// resolves to the native operators. Core is imported by every module, so flag enums compose via ADL at any
/// call site, template arguments included, with no per-file using declaration.
template <typename E>
    requires FlagEnum<E>
[[nodiscard]] constexpr auto operator|(E L, E R) noexcept -> E {
    return static_cast<E>(static_cast<std::underlying_type_t<E>>(L) | static_cast<std::underlying_type_t<E>>(R));
}

} // namespace SoulEngine
