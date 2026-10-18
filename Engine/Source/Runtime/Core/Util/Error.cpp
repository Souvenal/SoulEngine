module Core:Util.Error.impl;

import :Util.Error;

namespace SoulEngine {

ErrorMessage::ErrorMessage(String InError, std::source_location Loc) : m_Entries{{std::move(InError), Loc}} {}

auto ErrorMessage::Append(String InError, std::source_location Loc) const -> ErrorMessage {
    ErrorMessage Result = *this;
    Result.m_Entries.push_back({std::move(InError), Loc});
    return Result;
}

auto ErrorMessage::ToString() const -> String {
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

} // namespace SoulEngine
