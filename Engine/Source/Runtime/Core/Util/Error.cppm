/// @file   Core/Util/Error.cppm
/// @brief  Structured error chain type for std::expected error propagation.
///
/// Replaces bare `String` in `std::expected<T, String>` throughout the engine.
/// Each ErrorMessage holds an ordered chain of error messages from the
/// innermost error cause to the outermost context.  Call `.Append()` at each
/// layer to add context, then call `.ToString()` at the terminal to render the
/// chain as a progressively indented "Caused by:" tree.

export module Core:Util.Error;

import std;
import :Util.Types;

export namespace SoulEngine::Core {

/// @brief Structured error chain holding messages from innermost cause to
///        outermost context.
///
/// Construction:
///   ErrorMessage(Format("Leaf error"))                    — innermost (first) error
///   ErrorMessage("Leaf error with no format args")        — also works
///   R.error().Append(Format("Outer context with {}", x))  — add a layer
///   R.error().Append("Outer context (no format args)")    — also works
///
/// Rendering (outermost first, progressive "Caused by:" indentation,
///           followed by the leaf error source location):
///   LoadScene failed
///     Caused by: Vertex shader compilation failed
///       Caused by: Slang error
///   at Engine/Source/Runtime/Renderer/Renderer.cpp:42:15
class ErrorMessage {
  public:
    /// @brief Construct the innermost (leaf) error.
    /// No default constructor — an error must have content.
    /// @param Loc      Source location (auto-captured at call site).
    /// @param InError  Error message string.
    explicit ErrorMessage(String InError, std::source_location Loc = std::source_location::current())
        : m_Messages{std::move(InError)}, m_Location{Loc} {}

    /// @brief Append a layer of context and return a new ErrorMessage.
    /// Works on const lvalue references (e.g. `R.error().Append(...)`).
    [[nodiscard]] auto Append(String InError) const -> ErrorMessage {
        ErrorMessage Result = *this;
        Result.m_Messages.push_back(std::move(InError));
        return Result;
    }

    /// @brief Render the chain: outermost first, progressive "Caused by:" indentation,
    ///        followed by the leaf error source location.
    ///
    /// Example with 3 layers:
    ///   LoadScene failed
    ///     Caused by: Vertex shader compilation failed
    ///       Caused by: Slang error
    ///   at Engine/Source/Runtime/Renderer/Renderer.cpp:42:15
    [[nodiscard]] auto ToString() const -> String {
        String      Out   = m_Messages.back(); // outermost
        std::size_t Depth = 1;
        for (auto It = m_Messages.rbegin() + 1; It != m_Messages.rend(); ++It, ++Depth)
            Out += Format("\n{:{}}Caused by: {}", "", Depth * 2, *It);
        Out += Format("\nat {}:{}:{}", m_Location.file_name(), m_Location.line(), m_Location.column());
        return Out;
    }

  private:
    std::vector<String>  m_Messages;      // [innermost, ..., outermost]
    std::source_location m_Location = {}; // leaf error construction site
};

} // namespace SoulEngine::Core
