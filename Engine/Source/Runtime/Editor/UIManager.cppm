module;
#include <functional>
#include <string>
#include <unordered_map>

export module Editor:UIManager;

import std;
import Core;

export namespace SoulEngine {

/// @brief UI 注册项
struct UIEntry {
    std::function<void()> Callback;
    bool Show;
};

/// @brief UI 管理器（单例）
class UIManager final : public Singleton<UIManager> {
    friend class Singleton<UIManager>;

  public:
    /// @brief 注册一个 UI 项
    auto Register(std::string name, std::function<void()> callback, bool defaultShow = false) -> void {
        m_Entries[std::move(name)] = {std::move(callback), defaultShow};
    }

    /// @brief 打开指定 UI
    auto OpenUI(const std::string& name) -> void {
        if (auto it = m_Entries.find(name); it != m_Entries.end()) {
            it->second.Show = true;
        }
    }

    /// @brief 关闭指定 UI
    auto CloseUI(const std::string& name) -> void {
        if (auto it = m_Entries.find(name); it != m_Entries.end()) {
            it->second.Show = false;
        }
    }

    /// @brief 切换指定 UI 的显示状态
    auto ToggleUI(const std::string& name) -> void {
        if (auto it = m_Entries.find(name); it != m_Entries.end()) {
            it->second.Show = !it->second.Show;
        }
    }

    /// @brief 查询指定 UI 是否正在显示
    auto IsUIShowing(const std::string& name) const -> bool {
        if (auto it = m_Entries.find(name); it != m_Entries.end()) {
            return it->second.Show;
        }
        return false;
    }

    /// @brief 获取所有注册项（用于遍历）
    auto GetAll() -> std::unordered_map<std::string, UIEntry>& {
        return m_Entries;
    }

    auto GetAll() const -> const std::unordered_map<std::string, UIEntry>& {
        return m_Entries;
    }

  private:
    UIManager() = default;
    std::unordered_map<std::string, UIEntry> m_Entries;
};

} // namespace SoulEngine