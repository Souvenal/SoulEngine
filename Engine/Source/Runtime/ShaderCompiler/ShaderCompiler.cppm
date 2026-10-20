/// @file   ShaderCompiler.cppm
/// @brief  Main ShaderCompiler module — singleton facade over per-language
///         compiler backends.
///
/// Usage:
///   auto Result = ShaderCompiler::Get().CompileGraphics(Desc);
///
/// Thread safety: each compiler backend has its own mutex. Compiling concurrently; same-language compilations
/// are serialized.
///
/// ShaderBackend selection: explicit ShaderBackend enum in shader compile descriptors. File extension is
/// validated against the enum value (warning on mismatch) but never overrides it.
///
/// ShaderBackend registration: backend modules (e.g., Slang) self-register with ShaderBackendFactory via
/// AutoRegistrar constructors when the shared library is loaded. ShaderCompiler never references concrete
/// backend types directly. Adding a new backend requires zero changes to this file.

module;

export module ShaderCompiler;

export import :Types;
export import :Facade;
