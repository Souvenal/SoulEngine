/// @file   IO.cppm
/// @brief  File-system IO utilities.
module;

export module Core.Util.IO;

import std;
export import Core.Util.Types;
export import Core.Util.Error;

namespace SoulEngine {

/// @brief Read an entire file into a string (binary mode).
///
/// Uses `ate` to pre-allocate the string in one shot, avoiding repeated reallocation during sequential reads.
///
/// @returns File content on success, or an error message on failure
///          (file not found, permission denied, read error, etc.).
///
/// Note: deliberately non-inline. MSVC 14.51 fails to materialize this body in classic translation units that
/// import Core (the nested basic_istream<char>::sentry is incomplete when tellg() is instantiated there), so
/// the body is compiled only in this module unit.
export [[nodiscard]] auto ReadFile(const Path& FileName) -> std::expected<String, ErrorMessage>;

} // namespace SoulEngine
