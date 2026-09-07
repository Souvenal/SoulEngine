/// @file   IO.cppm
/// @brief  File-system IO utilities.
module;

export module Core:Util.IO;

export import std;
export import :Util.Types;
export import :Util.Error;

export namespace SoulEngine {

/// @brief Read an entire file into a string (binary mode).
///
/// Uses `ate` to pre-allocate the string in one shot, avoiding repeated
/// reallocation during sequential reads.
///
/// @returns File content on success, or an error message on failure
///          (file not found, permission denied, read error, etc.).
///
/// Note: deliberately non-inline. MSVC 14.51 fails to materialize this
/// body in classic translation units that import Core (the nested
/// basic_istream<char>::sentry is incomplete when tellg() is instantiated
/// there), so the body is compiled only in this module unit.
[[nodiscard]] auto ReadFile(const Path& FileName) -> std::expected<String, ErrorMessage> {
    std::ifstream File(FileName, std::ios::binary | std::ios::ate);
    if (!File)
        return std::unexpected(ErrorMessage(Format("Cannot open file '{}'", FileName.string())));

    auto Size = File.tellg();
    if (Size == -1)
        return std::unexpected(ErrorMessage(Format("Failed to determine size of '{}'", FileName.string())));

    File.seekg(0);

    String Content(static_cast<std::string::size_type>(Size), '\0');
    if (!File.read(Content.data(), Size))
        return std::unexpected(ErrorMessage(Format("Failed to read '{}'", FileName.string())));

    return Content;
}

} // namespace SoulEngine
