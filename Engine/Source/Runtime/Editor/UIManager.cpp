module;

module Editor.UIManager;

import std;

namespace SoulEngine {

auto UIManager::Register(std::string name, std::function<void()> callback, bool defaultShow) -> void {
    m_Entries[std::move(name)] = {std::move(callback), defaultShow};
}

auto UIManager::OpenUI(const std::string& name) -> void {
    if (auto it = m_Entries.find(name); it != m_Entries.end()) {
        it->second.Show = true;
    }
}

auto UIManager::CloseUI(const std::string& name) -> void {
    if (auto it = m_Entries.find(name); it != m_Entries.end()) {
        it->second.Show = false;
    }
}

auto UIManager::ToggleUI(const std::string& name) -> void {
    if (auto it = m_Entries.find(name); it != m_Entries.end()) {
        it->second.Show = !it->second.Show;
    }
}

auto UIManager::IsUIShowing(const std::string& name) const -> bool {
    if (auto it = m_Entries.find(name); it != m_Entries.end()) {
        return it->second.Show;
    }
    return false;
}

auto UIManager::GetAll() -> std::unordered_map<std::string, UIEntry>& {
    return m_Entries;
}

auto UIManager::GetAll() const -> const std::unordered_map<std::string, UIEntry>& {
    return m_Entries;
}

} // namespace SoulEngine
