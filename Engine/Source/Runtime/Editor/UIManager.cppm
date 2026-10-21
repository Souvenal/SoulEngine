module;

export module Editor.UIManager;

import std;
import Core;

namespace SoulEngine {

/// @brief UI 注册项
export struct UIEntry {
    std::function<void()> Callback;
    bool                  Show;
};

/// @brief UI 管理器（单例）
export class UIManager final : public Singleton<UIManager> {
    friend class Singleton<UIManager>;

  public:
    /// @brief 注册一个 UI 项
    auto Register(std::string name, std::function<void()> callback, bool defaultShow = false) -> void;

    /// @brief 打开指定 UI
    auto OpenUI(const std::string& name) -> void;

    /// @brief 关闭指定 UI
    auto CloseUI(const std::string& name) -> void;

    /// @brief 切换指定 UI 的显示状态
    auto ToggleUI(const std::string& name) -> void;

    /// @brief 查询指定 UI 是否正在显示
    auto IsUIShowing(const std::string& name) const -> bool;

    /// @brief 获取所有注册项（用于遍历）
    auto GetAll() -> std::unordered_map<std::string, UIEntry>&;

    auto GetAll() const -> const std::unordered_map<std::string, UIEntry>&;

  private:
    UIManager() = default;
    std::unordered_map<std::string, UIEntry> m_Entries;
};

} // namespace SoulEngine
