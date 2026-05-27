/// @file   IO.cppm
/// @brief  File-system IO utilities.

export module Core:Util.IO;

export import :Util.Types;
export import :Util.Error;
export import std;

export namespace SoulEngine::Core {

/// @brief Read an entire file into a string (binary mode).
///
/// Uses `ate` to pre-allocate the string in one shot, avoiding repeated
/// reallocation during sequential reads.
///
/// @returns File content on success, or an error message on failure
///          (file not found, permission denied, read error, etc.).
[[nodiscard]] inline auto ReadFile(const Path& FileName) -> std::expected<String, ErrorMessage> {
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

} // namespace SoulEngine::Core
