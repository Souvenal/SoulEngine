/// @file   Core/Util/Error.cppm
/// @brief  Structured error chain type for std::expected error propagation.
///
/// Replaces bare `String` in `std::expected<T, String>` throughout the engine.
/// Each ErrorMessage holds an ordered chain of error messages from the
/// innermost error cause to the outermost context, with a source location
/// recorded per layer.  Call `.Append()` at each layer to add context, then
/// call `.ToString()` at the terminal to render the chain as a progressively
/// indented "Caused by:" tree with per-layer source locations.

export module Core:Util.Error;

import std;
import :Util.Types;

export namespace SoulEngine {

/// @brief Structured error chain holding messages from innermost cause to
///        outermost context, each with its construction-site source location.
///
/// Construction:
///   ErrorMessage(Format("Leaf error"))                    — innermost (first) error
///   ErrorMessage("Leaf error with no format args")        — also works
///   R.error().Append(Format("Outer context with {}", x))  — add a layer
///   R.error().Append("Outer context (no format args)")    — also works
///
/// Rendering (outermost first, progressive "Caused by:" indentation, each
///           layer's source location on a following line, aligned with that
///           layer's indentation):
///   LoadScene failed
///   [at Engine/Source/Runtime/Launch/Main.cpp:10]
///     Caused by: Vertex shader compilation failed
///     [at Engine/Source/Runtime/Renderer/Renderer.cpp:87]
///       Caused by: Slang error
///       [at Engine/Source/Runtime/ShaderCompiler/Compiler.cpp:42]
class ErrorMessage {
  public:
    /// @brief Construct the innermost (leaf) error.
    /// No default constructor — an error must have content.
    /// @param InError  Error message string.
    /// @param Loc      Source location (auto-captured at call site).
    explicit ErrorMessage(String InError, std::source_location Loc = std::source_location::current())
        : m_Entries{{std::move(InError), Loc}} {}

    /// @brief Append a layer of context and return a new ErrorMessage.
    /// Works on const lvalue references (e.g. `R.error().Append(...)`).
    /// @param InError  Context message string.
    /// @param Loc      Source location (auto-captured at call site).
    [[nodiscard]] auto Append(String InError, std::source_location Loc = std::source_location::current()) const
        -> ErrorMessage {
        ErrorMessage Result = *this;
        Result.m_Entries.push_back({std::move(InError), Loc});
        return Result;
    }

    /// @brief Render the chain: outermost first, progressive "Caused by:"
    ///        indentation, each layer's source location on a following line
    ///        aligned with that layer's indentation.
    ///
    /// Example with 3 layers:
    ///   Application init failed
    ///   [at Engine/Source/Runtime/Launch/Main.cpp:10]
    ///     Caused by: Vertex shader compilation failed
    ///     [at Engine/Source/Runtime/Renderer/Renderer.cpp:87]
    ///       Caused by: Slang compile failed
    ///       [at Engine/Source/Runtime/ShaderCompiler/Compiler.cpp:42]
    [[nodiscard]] auto ToString() const -> String {
        String      Out;
        std::size_t Depth = 0;
        for (auto It = m_Entries.rbegin(); It != m_Entries.rend(); ++It, ++Depth) {
            if (Depth > 0)
                Out += '\n';
            Out += Format("{:{}}", "", Depth * 2);
            if (Depth > 0)
                Out += "Caused by: ";
            Out += It->Message;
            Out += Format("\n{:{}}[at {}:{}]", "", Depth * 2, It->Location.file_name(), It->Location.line());
        }
        return Out;
    }

  private:
    struct Entry {
        String               Message;
        std::source_location Location;
    };

    std::vector<Entry> m_Entries; // [innermost, ..., outermost]
};

} // namespace SoulEngine
