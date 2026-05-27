export module Core:Util.Types;

export import std;

export namespace SoulEngine::Core {

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

} // namespace SoulEngine::Core
