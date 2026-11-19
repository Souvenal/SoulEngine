// Source: Dear ImGui backends/imgui_impl_glfw.cpp at e2e9533991233d598459f03714fe7f732168813e.
// WindowSystem.Input supplies all mouse and keyboard events used by this adapter.

// dear imgui: Platform Backend for GLFW
//  This needs to be used along with a Renderer (e.g. OpenGL3, Vulkan, WebGPU..)
//  (Info: GLFW is a cross-platform general purpose library for handling windows, inputs,
//  OpenGL/Vulkan graphics context creation, etc.)
//  (Requires: GLFW 3.0+. Prefer GLFW 3.3+/3.4+ for full feature support.)

// Implemented features:
//  [X] Platform: Clipboard support.
//  [X] Platform: Mouse support.
// [X] Platform: Keyboard support. Since 1.87 we are using the io.AddKeyEvent() function. Pass ImGuiKey values
// to all key functions e.g. ImGui::IsKeyPressed(ImGuiKey_Space). [Legacy GLFW_KEY_* values are obsolete since
// 1.87 and not supported since 1.91.5]
//  [X] Platform: Gamepad support. Enable with 'io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad'.
// [X] Platform: Mouse cursor shape and visibility (ImGuiBackendFlags_HasMouseCursors) with GLFW 3.1+.
// Resizing cursors requires GLFW 3.4+! Disable with 'io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange'.
//  [X] Multiple Dear ImGui contexts support.

// You can use unmodified imgui_impl_* files in your project. See examples/ folder for examples of using this.
// Prefer including the entire imgui/ repository into your project (either as a copy or as a submodule), and
// only build the backends you need. Learn about Dear ImGui:
// - FAQ                  https://dearimgui.com/faq
// - Getting Started      https://dearimgui.com/getting-started
// - Documentation        https://dearimgui.com/docs (same as your local docs/ folder).
// - Introduction, links and more at the top of imgui.cpp

// About Emscripten support:
// - Emscripten provides its own GLFW (3.2.1) implementation (syntax: "-sUSE_GLFW=3"), but Joystick is broken
// and several features are not supported (multiple windows, clipboard, timer, etc.)
// - A third-party Emscripten GLFW (3.4.0) implementation (syntax: "--use-port=contrib.glfw3") fixes the
// Joystick issue and implements all relevant features for the browser. See
// https://github.com/pongasoft/emscripten-glfw/blob/master/docs/Comparison.md for details.

module;

#include "imgui.h"

// Clang warnings with -Weverything
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wold-style-cast"  // warning: use of old-style cast
#pragma clang diagnostic ignored "-Wsign-conversion" // warning: implicit conversion changes signedness
#pragma clang diagnostic ignored                                                                                       \
    "-Wexit-time-destructors" // warning: declaration requires an exit-time destructor     // exit-time destruction order is undefined. if MemFree() leads to users code that has been disabled before exit it might cause problems. ImGui coding style welcomes static/globals.
#pragma clang diagnostic ignored                                                                                       \
    "-Wglobal-constructors" // warning: declaration requires a global destructor         // similar to above, not sure what the exact difference is.
#endif

#if defined(__has_include)
#if !__has_include(<X11/Xlib.h>) || !__has_include(<X11/extensions/Xrandr.h>)
#define IMGUI_IMPL_GLFW_DISABLE_X11
#endif
#if !__has_include(<wayland-client.h>)
#define IMGUI_IMPL_GLFW_DISABLE_WAYLAND
#endif
#endif

// GLFW
#if !defined(IMGUI_IMPL_GLFW_DISABLE_X11) && (defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__) ||    \
                                              defined(__NetBSD__) || defined(__DragonFly__))
#define GLFW_HAS_X11 1
#else
#define GLFW_HAS_X11 0
#endif
#if !defined(IMGUI_IMPL_GLFW_DISABLE_WAYLAND) &&                                                                       \
    (defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) ||                      \
     defined(__DragonFly__))
#define GLFW_HAS_WAYLAND 1
#else
#define GLFW_HAS_WAYLAND 0
#endif
#include <GLFW/glfw3.h>
#ifdef _WIN32
#undef APIENTRY
#ifndef GLFW_EXPOSE_NATIVE_WIN32 // for glfwGetWin32Window()
#define GLFW_EXPOSE_NATIVE_WIN32
#endif
#include <GLFW/glfw3native.h>
#elif defined(__APPLE__)
#ifndef GLFW_EXPOSE_NATIVE_COCOA // for glfwGetCocoaWindow()
#define GLFW_EXPOSE_NATIVE_COCOA
#endif
#include <GLFW/glfw3native.h>
#elif GLFW_HAS_X11
#ifndef GLFW_EXPOSE_NATIVE_X11 // for glfwGetX11Display(), glfwGetX11Window() on Freedesktop (Linux, BSD, etc.)
#define GLFW_EXPOSE_NATIVE_X11
#endif
#include <GLFW/glfw3native.h>
#endif
#undef Status // X11 headers are leaking this.
#ifndef _WIN32
#include <unistd.h> // for usleep()
#endif
#include <stdio.h> // for snprintf()

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/html5.h>
#ifdef EMSCRIPTEN_USE_PORT_CONTRIB_GLFW3
#include <GLFW/emscripten_glfw3.h>
#else
#define EMSCRIPTEN_USE_EMBEDDED_GLFW3
#endif
#endif

module WindowSystem.ImGuiGlfwBackend;

import std;
import Core;
import WindowSystem.Types;

namespace SoulEngine {
namespace ImGuiGlfwAdapter {

// We gather version tests as define in order to easily see which features are version-dependent.
#define GLFW_VERSION_COMBINED (GLFW_VERSION_MAJOR * 1000 + GLFW_VERSION_MINOR * 100 + GLFW_VERSION_REVISION)
#define GLFW_HAS_CREATECURSOR (GLFW_VERSION_COMBINED >= 3100)    // 3.1+ glfwCreateCursor()
#define GLFW_HAS_PER_MONITOR_DPI (GLFW_VERSION_COMBINED >= 3300) // 3.3+ glfwGetMonitorContentScale
#ifdef GLFW_RESIZE_NESW_CURSOR // Let's be nice to people who pulled GLFW between 2019-04-16 (3.4 define) and 2019-11-29 (cursors defines) // FIXME: Remove when GLFW 3.4 is released?
#define GLFW_HAS_NEW_CURSORS                                                                                           \
    (GLFW_VERSION_COMBINED >=                                                                                          \
     3400) // 3.4+ GLFW_RESIZE_ALL_CURSOR, GLFW_RESIZE_NESW_CURSOR, GLFW_RESIZE_NWSE_CURSOR, GLFW_NOT_ALLOWED_CURSOR
#else
#define GLFW_HAS_NEW_CURSORS (0)
#endif
#define GLFW_HAS_GAMEPAD_API (GLFW_VERSION_COMBINED >= 3300) // 3.3+ glfwGetGamepadState() new api
#define GLFW_HAS_GETKEYNAME (GLFW_VERSION_COMBINED >= 3200)  // 3.2+ glfwGetKeyName()
#define GLFW_HAS_GETERROR (GLFW_VERSION_COMBINED >= 3300)    // 3.3+ glfwGetError()
#define GLFW_HAS_GETPLATFORM (GLFW_VERSION_COMBINED >= 3400) // 3.4+ glfwGetPlatform()

enum GlfwClientApi {
    GlfwClientApi_OpenGL,
    GlfwClientApi_Vulkan,
    GlfwClientApi_Unknown, // Anything else fits here.
};

#if GLFW_HAS_CREATECURSOR
class ImGuiGlfwCursorOwner {
  public:
    ImGuiGlfwCursorOwner() = default;
    ~ImGuiGlfwCursorOwner() {
        Reset();
    }

    ImGuiGlfwCursorOwner(const ImGuiGlfwCursorOwner&)                    = delete;
    auto operator=(const ImGuiGlfwCursorOwner&) -> ImGuiGlfwCursorOwner& = delete;
    ImGuiGlfwCursorOwner(ImGuiGlfwCursorOwner&&)                         = delete;
    auto operator=(ImGuiGlfwCursorOwner&&) -> ImGuiGlfwCursorOwner&      = delete;

    auto Reset(GLFWcursor* Cursor = nullptr) -> void {
        if (m_Cursor) {
            // NOLINT-RAW-MEM: The owner pairs glfwCreateStandardCursor with glfwDestroyCursor.
            glfwDestroyCursor(m_Cursor);
        }
        m_Cursor = Cursor;
    }

    [[nodiscard]] auto Get() const -> GLFWcursor* {
        return m_Cursor;
    }

  private:
    GLFWcursor* m_Cursor = nullptr;
};
#endif

// GLFW data
struct ImGui_ImplGlfw_Data {
    ImGuiContext* Context   = nullptr;
    GLFWwindow*   Window    = nullptr;
    GlfwClientApi ClientApi = GlfwClientApi_Unknown;
    double        Time      = 0.0;
#if GLFW_HAS_CREATECURSOR
    ImGuiGlfwCursorOwner MouseCursors[ImGuiMouseCursor_COUNT] = {};
    GLFWcursor*          LastMouseCursor                      = nullptr;
#endif
    bool IsWayland               = false;
    char BackendPlatformName[40] = {};
#ifdef EMSCRIPTEN_USE_EMBEDDED_GLFW3
    const char* CanvasSelector = nullptr;
#endif
};

class ImGuiGlfwDataOwner {
  public:
    explicit ImGuiGlfwDataOwner(ImGui_ImplGlfw_Data* Data = nullptr) : m_Data(Data) {}
    ~ImGuiGlfwDataOwner() {
        Reset();
    }

    ImGuiGlfwDataOwner(const ImGuiGlfwDataOwner&)                    = delete;
    auto operator=(const ImGuiGlfwDataOwner&) -> ImGuiGlfwDataOwner& = delete;
    ImGuiGlfwDataOwner(ImGuiGlfwDataOwner&&)                         = delete;
    auto operator=(ImGuiGlfwDataOwner&&) -> ImGuiGlfwDataOwner&      = delete;

    [[nodiscard]] static auto Create() -> std::expected<UPtr<ImGuiGlfwDataOwner>, ErrorMessage> {
        auto Result    = std::make_unique<ImGuiGlfwDataOwner>();
        // NOLINT-RAW-MEM: Dear ImGui backend storage uses its matching allocator and placement constructor.
        Result->m_Data = IM_NEW(ImGui_ImplGlfw_Data)();
        if (!Result->m_Data)
            return std::unexpected(ErrorMessage("ImGui GLFW backend allocation failed"));
        return Result;
    }

    [[nodiscard]] auto Get() const -> ImGui_ImplGlfw_Data* {
        return m_Data;
    }

    [[nodiscard]] auto Release() -> ImGui_ImplGlfw_Data* {
        return std::exchange(m_Data, nullptr);
    }

  private:
    auto Reset() -> void {
        if (m_Data) {
            // NOLINT-RAW-MEM: The owner pairs Dear ImGui's placement destructor with its allocator.
            IM_DELETE(m_Data);
        }
        m_Data = nullptr;
    }

    ImGui_ImplGlfw_Data* m_Data = nullptr;
};

// Backend data stored in io.BackendPlatformUserData to allow support for multiple Dear ImGui contexts It is
// STRONGLY preferred that you use docking branch with multi-viewports (== single Dear ImGui context +
// multiple windows) instead of multiple Dear ImGui contexts. FIXME: multi-context support is not well tested
// and probably dysfunctional in this backend.
// - WindowSystem owns native event collection and supplies executed input through ApplyInput().
// - Otherwise we may need to store a GLFWWindow* -> ImGuiContext* map and handle this in the backend, adding
// a little bit of extra complexity to it. FIXME: some shared resources (mouse cursor shape, gamepad) are
// mishandled when using multi-context.
static auto ImGui_ImplGlfw_GetBackendData() -> ImGui_ImplGlfw_Data* {
    // Get data for current context
    return ImGui::GetCurrentContext() ? (ImGui_ImplGlfw_Data*)ImGui::GetIO().BackendPlatformUserData : nullptr;
}
// Functions
static auto ImGui_ImplGlfw_IsWayland() -> bool {
#if !GLFW_HAS_WAYLAND
    return false;
#elif GLFW_HAS_GETPLATFORM
    return glfwGetPlatform() == GLFW_PLATFORM_WAYLAND;
#else
    const char* version = glfwGetVersionString();
    if (strstr(version, "Wayland") == nullptr) // e.g. Ubuntu 22.04 ships with GLFW 3.3.6 compiled without Wayland
        return false;
#ifdef GLFW_EXPOSE_NATIVE_X11
    if (glfwGetX11Display() != nullptr)
        return false;
#endif
    return true;
#endif
}

// Map translated GLFW keys to ImGui keys.
auto ImGui_ImplGlfw_KeyToImGuiKey(int keycode, int scancode) -> ImGuiKey {
    IM_UNUSED(scancode);
    switch (keycode) {
    case GLFW_KEY_TAB:
        return ImGuiKey_Tab;
    case GLFW_KEY_LEFT:
        return ImGuiKey_LeftArrow;
    case GLFW_KEY_RIGHT:
        return ImGuiKey_RightArrow;
    case GLFW_KEY_UP:
        return ImGuiKey_UpArrow;
    case GLFW_KEY_DOWN:
        return ImGuiKey_DownArrow;
    case GLFW_KEY_PAGE_UP:
        return ImGuiKey_PageUp;
    case GLFW_KEY_PAGE_DOWN:
        return ImGuiKey_PageDown;
    case GLFW_KEY_HOME:
        return ImGuiKey_Home;
    case GLFW_KEY_END:
        return ImGuiKey_End;
    case GLFW_KEY_INSERT:
        return ImGuiKey_Insert;
    case GLFW_KEY_DELETE:
        return ImGuiKey_Delete;
    case GLFW_KEY_BACKSPACE:
        return ImGuiKey_Backspace;
    case GLFW_KEY_SPACE:
        return ImGuiKey_Space;
    case GLFW_KEY_ENTER:
        return ImGuiKey_Enter;
    case GLFW_KEY_ESCAPE:
        return ImGuiKey_Escape;
    case GLFW_KEY_APOSTROPHE:
        return ImGuiKey_Apostrophe;
    case GLFW_KEY_COMMA:
        return ImGuiKey_Comma;
    case GLFW_KEY_MINUS:
        return ImGuiKey_Minus;
    case GLFW_KEY_PERIOD:
        return ImGuiKey_Period;
    case GLFW_KEY_SLASH:
        return ImGuiKey_Slash;
    case GLFW_KEY_SEMICOLON:
        return ImGuiKey_Semicolon;
    case GLFW_KEY_EQUAL:
        return ImGuiKey_Equal;
    case GLFW_KEY_LEFT_BRACKET:
        return ImGuiKey_LeftBracket;
    case GLFW_KEY_BACKSLASH:
        return ImGuiKey_Backslash;
    case GLFW_KEY_WORLD_1:
        return ImGuiKey_Oem102;
    case GLFW_KEY_WORLD_2:
        return ImGuiKey_Oem102;
    case GLFW_KEY_RIGHT_BRACKET:
        return ImGuiKey_RightBracket;
    case GLFW_KEY_GRAVE_ACCENT:
        return ImGuiKey_GraveAccent;
    case GLFW_KEY_CAPS_LOCK:
        return ImGuiKey_CapsLock;
    case GLFW_KEY_SCROLL_LOCK:
        return ImGuiKey_ScrollLock;
    case GLFW_KEY_NUM_LOCK:
        return ImGuiKey_NumLock;
    case GLFW_KEY_PRINT_SCREEN:
        return ImGuiKey_PrintScreen;
    case GLFW_KEY_PAUSE:
        return ImGuiKey_Pause;
    case GLFW_KEY_KP_0:
        return ImGuiKey_Keypad0;
    case GLFW_KEY_KP_1:
        return ImGuiKey_Keypad1;
    case GLFW_KEY_KP_2:
        return ImGuiKey_Keypad2;
    case GLFW_KEY_KP_3:
        return ImGuiKey_Keypad3;
    case GLFW_KEY_KP_4:
        return ImGuiKey_Keypad4;
    case GLFW_KEY_KP_5:
        return ImGuiKey_Keypad5;
    case GLFW_KEY_KP_6:
        return ImGuiKey_Keypad6;
    case GLFW_KEY_KP_7:
        return ImGuiKey_Keypad7;
    case GLFW_KEY_KP_8:
        return ImGuiKey_Keypad8;
    case GLFW_KEY_KP_9:
        return ImGuiKey_Keypad9;
    case GLFW_KEY_KP_DECIMAL:
        return ImGuiKey_KeypadDecimal;
    case GLFW_KEY_KP_DIVIDE:
        return ImGuiKey_KeypadDivide;
    case GLFW_KEY_KP_MULTIPLY:
        return ImGuiKey_KeypadMultiply;
    case GLFW_KEY_KP_SUBTRACT:
        return ImGuiKey_KeypadSubtract;
    case GLFW_KEY_KP_ADD:
        return ImGuiKey_KeypadAdd;
    case GLFW_KEY_KP_ENTER:
        return ImGuiKey_KeypadEnter;
    case GLFW_KEY_KP_EQUAL:
        return ImGuiKey_KeypadEqual;
    case GLFW_KEY_LEFT_SHIFT:
        return ImGuiKey_LeftShift;
    case GLFW_KEY_LEFT_CONTROL:
        return ImGuiKey_LeftCtrl;
    case GLFW_KEY_LEFT_ALT:
        return ImGuiKey_LeftAlt;
    case GLFW_KEY_LEFT_SUPER:
        return ImGuiKey_LeftSuper;
    case GLFW_KEY_RIGHT_SHIFT:
        return ImGuiKey_RightShift;
    case GLFW_KEY_RIGHT_CONTROL:
        return ImGuiKey_RightCtrl;
    case GLFW_KEY_RIGHT_ALT:
        return ImGuiKey_RightAlt;
    case GLFW_KEY_RIGHT_SUPER:
        return ImGuiKey_RightSuper;
    case GLFW_KEY_MENU:
        return ImGuiKey_Menu;
    case GLFW_KEY_0:
        return ImGuiKey_0;
    case GLFW_KEY_1:
        return ImGuiKey_1;
    case GLFW_KEY_2:
        return ImGuiKey_2;
    case GLFW_KEY_3:
        return ImGuiKey_3;
    case GLFW_KEY_4:
        return ImGuiKey_4;
    case GLFW_KEY_5:
        return ImGuiKey_5;
    case GLFW_KEY_6:
        return ImGuiKey_6;
    case GLFW_KEY_7:
        return ImGuiKey_7;
    case GLFW_KEY_8:
        return ImGuiKey_8;
    case GLFW_KEY_9:
        return ImGuiKey_9;
    case GLFW_KEY_A:
        return ImGuiKey_A;
    case GLFW_KEY_B:
        return ImGuiKey_B;
    case GLFW_KEY_C:
        return ImGuiKey_C;
    case GLFW_KEY_D:
        return ImGuiKey_D;
    case GLFW_KEY_E:
        return ImGuiKey_E;
    case GLFW_KEY_F:
        return ImGuiKey_F;
    case GLFW_KEY_G:
        return ImGuiKey_G;
    case GLFW_KEY_H:
        return ImGuiKey_H;
    case GLFW_KEY_I:
        return ImGuiKey_I;
    case GLFW_KEY_J:
        return ImGuiKey_J;
    case GLFW_KEY_K:
        return ImGuiKey_K;
    case GLFW_KEY_L:
        return ImGuiKey_L;
    case GLFW_KEY_M:
        return ImGuiKey_M;
    case GLFW_KEY_N:
        return ImGuiKey_N;
    case GLFW_KEY_O:
        return ImGuiKey_O;
    case GLFW_KEY_P:
        return ImGuiKey_P;
    case GLFW_KEY_Q:
        return ImGuiKey_Q;
    case GLFW_KEY_R:
        return ImGuiKey_R;
    case GLFW_KEY_S:
        return ImGuiKey_S;
    case GLFW_KEY_T:
        return ImGuiKey_T;
    case GLFW_KEY_U:
        return ImGuiKey_U;
    case GLFW_KEY_V:
        return ImGuiKey_V;
    case GLFW_KEY_W:
        return ImGuiKey_W;
    case GLFW_KEY_X:
        return ImGuiKey_X;
    case GLFW_KEY_Y:
        return ImGuiKey_Y;
    case GLFW_KEY_Z:
        return ImGuiKey_Z;
    case GLFW_KEY_F1:
        return ImGuiKey_F1;
    case GLFW_KEY_F2:
        return ImGuiKey_F2;
    case GLFW_KEY_F3:
        return ImGuiKey_F3;
    case GLFW_KEY_F4:
        return ImGuiKey_F4;
    case GLFW_KEY_F5:
        return ImGuiKey_F5;
    case GLFW_KEY_F6:
        return ImGuiKey_F6;
    case GLFW_KEY_F7:
        return ImGuiKey_F7;
    case GLFW_KEY_F8:
        return ImGuiKey_F8;
    case GLFW_KEY_F9:
        return ImGuiKey_F9;
    case GLFW_KEY_F10:
        return ImGuiKey_F10;
    case GLFW_KEY_F11:
        return ImGuiKey_F11;
    case GLFW_KEY_F12:
        return ImGuiKey_F12;
    case GLFW_KEY_F13:
        return ImGuiKey_F13;
    case GLFW_KEY_F14:
        return ImGuiKey_F14;
    case GLFW_KEY_F15:
        return ImGuiKey_F15;
    case GLFW_KEY_F16:
        return ImGuiKey_F16;
    case GLFW_KEY_F17:
        return ImGuiKey_F17;
    case GLFW_KEY_F18:
        return ImGuiKey_F18;
    case GLFW_KEY_F19:
        return ImGuiKey_F19;
    case GLFW_KEY_F20:
        return ImGuiKey_F20;
    case GLFW_KEY_F21:
        return ImGuiKey_F21;
    case GLFW_KEY_F22:
        return ImGuiKey_F22;
    case GLFW_KEY_F23:
        return ImGuiKey_F23;
    case GLFW_KEY_F24:
        return ImGuiKey_F24;
    default:
        return ImGuiKey_None;
    }
}

#ifdef EMSCRIPTEN_USE_EMBEDDED_GLFW3
static auto ImGui_ImplEmscripten_WheelCallback(int, const EmscriptenWheelEvent* ev, void* user_data) -> EM_BOOL {
    // Mimic Emscripten_HandleWheel() in SDL. Corresponding equivalent in GLFW JS emulation layer has
    // incorrect quantizing preventing small values. See #6096
    ImGui_ImplGlfw_Data* bd         = (ImGui_ImplGlfw_Data*)user_data;
    float                multiplier = 0.0f;
    if (ev->deltaMode == DOM_DELTA_PIXEL) {
        multiplier = 1.0f / 100.0f;
    } // 100 pixels make up a step.
    else if (ev->deltaMode == DOM_DELTA_LINE) {
        multiplier = 1.0f / 3.0f;
    } // 3 lines make up a step.
    else if (ev->deltaMode == DOM_DELTA_PAGE) {
        multiplier = 80.0f;
    } // A page makes up 80 steps.
    float    wheel_x = ev->deltaX * -multiplier;
    float    wheel_y = ev->deltaY * -multiplier;
    ImGuiIO& io      = ImGui::GetIO(bd->Context);
    io.AddMouseWheelEvent(wheel_x, wheel_y);
    // IMGUI_DEBUG_LOG("[Emsc] mode %d dx: %.2f, dy: %.2f, dz: %.2f --> feed %.2f %.2f\n", (int)ev->deltaMode,
    // ev->deltaX, ev->deltaY, ev->deltaZ, wheel_x, wheel_y);
    return EM_TRUE;
}
#endif

#ifdef __EMSCRIPTEN__
#if EMSCRIPTEN_USE_PORT_CONTRIB_GLFW3 >= 34020240817
auto ImGui_ImplGlfw_EmscriptenOpenURL(const char* url) -> void {
    if (url)
        emscripten::glfw3::OpenURL(url);
}
#else
EM_JS(void, ImGui_ImplGlfw_EmscriptenOpenURL, (const char* url), {
    url = url ? UTF8ToString(url) : null;
    if (url)
        window.open(url, '_blank');
});
#endif
#endif

static auto ImGui_ImplGlfw_Init(GLFWwindow* window, GlfwClientApi client_api) -> std::expected<void, ErrorMessage> {
    ImGuiIO& io = ImGui::GetIO();
    IMGUI_CHECKVERSION();
    IM_ASSERT(io.BackendPlatformUserData == nullptr && "Already initialized a platform backend!");
    // printf("GLFW_VERSION: %d.%d.%d (%d)", GLFW_VERSION_MAJOR, GLFW_VERSION_MINOR, GLFW_VERSION_REVISION,
    // GLFW_VERSION_COMBINED);

    // Setup backend capabilities flags
    auto DataOwner = ImGuiGlfwDataOwner::Create();
    if (!DataOwner)
        return std::unexpected(DataOwner.error().Append("ImGui GLFW backend initialization failed"));
    ImGui_ImplGlfw_Data* bd = (*DataOwner)->Get();
    bd->Context             = ImGui::GetCurrentContext();
    bd->Window              = window;
    bd->Time                = 0.0;
    bd->IsWayland           = ImGui_ImplGlfw_IsWayland();

    snprintf(bd->BackendPlatformName,
             sizeof(bd->BackendPlatformName),
             "imgui_impl_glfw (%d)%s",
             GLFW_VERSION_COMBINED,
             bd->IsWayland ? " (Wayland)" : "");
    io.BackendPlatformUserData = (void*)bd;
    io.BackendPlatformName     = bd->BackendPlatformName;
#if GLFW_HAS_CREATECURSOR
    io.BackendFlags |= ImGuiBackendFlags_HasMouseCursors; // We can honor GetMouseCursor() values (optional)
#endif
    io.BackendFlags |=
        ImGuiBackendFlags_HasSetMousePos; // We can honor io.WantSetMousePos requests (optional, rarely used)

    ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();
#if GLFW_VERSION_COMBINED < 3300
    platform_io.Platform_SetClipboardTextFn = [](ImGuiContext*, const char* text) -> void {
        glfwSetClipboardString(ImGui_ImplGlfw_GetBackendData()->Window, text);
    };
    platform_io.Platform_GetClipboardTextFn = [](ImGuiContext*) -> const char* {
        return glfwGetClipboardString(ImGui_ImplGlfw_GetBackendData()->Window);
    };
#else
    platform_io.Platform_SetClipboardTextFn = [](ImGuiContext*, const char* text) -> void {
        glfwSetClipboardString(nullptr, text);
    };
    platform_io.Platform_GetClipboardTextFn = [](ImGuiContext*) -> const char* {
        return glfwGetClipboardString(nullptr);
    };
#endif

#ifdef __EMSCRIPTEN__
    platform_io.Platform_OpenInShellFn = [](ImGuiContext*, const char* url) -> bool {
        ImGui_ImplGlfw_EmscriptenOpenURL(url);
        return true;
    };
#endif

    // Create mouse cursors (By design, on X11 cursors are user configurable and some cursors may be missing.
    // When a cursor doesn't exist, GLFW will emit an error which will often be printed by the app, so we
    // temporarily disable error reporting. Missing cursors will return nullptr and our _UpdateMouseCursor()
    // function will use the Arrow cursor instead.)
#if GLFW_HAS_CREATECURSOR
    GLFWerrorfun prev_error_callback = glfwSetErrorCallback(nullptr);
    bd->MouseCursors[ImGuiMouseCursor_Arrow].Reset(glfwCreateStandardCursor(GLFW_ARROW_CURSOR));
    bd->MouseCursors[ImGuiMouseCursor_TextInput].Reset(glfwCreateStandardCursor(GLFW_IBEAM_CURSOR));
    bd->MouseCursors[ImGuiMouseCursor_ResizeNS].Reset(glfwCreateStandardCursor(GLFW_VRESIZE_CURSOR));
    bd->MouseCursors[ImGuiMouseCursor_ResizeEW].Reset(glfwCreateStandardCursor(GLFW_HRESIZE_CURSOR));
    bd->MouseCursors[ImGuiMouseCursor_Hand].Reset(glfwCreateStandardCursor(GLFW_HAND_CURSOR));
#if GLFW_HAS_NEW_CURSORS
    bd->MouseCursors[ImGuiMouseCursor_ResizeAll].Reset(glfwCreateStandardCursor(GLFW_RESIZE_ALL_CURSOR));
    bd->MouseCursors[ImGuiMouseCursor_ResizeNESW].Reset(glfwCreateStandardCursor(GLFW_RESIZE_NESW_CURSOR));
    bd->MouseCursors[ImGuiMouseCursor_ResizeNWSE].Reset(glfwCreateStandardCursor(GLFW_RESIZE_NWSE_CURSOR));
    bd->MouseCursors[ImGuiMouseCursor_NotAllowed].Reset(glfwCreateStandardCursor(GLFW_NOT_ALLOWED_CURSOR));
#else
    bd->MouseCursors[ImGuiMouseCursor_ResizeAll].Reset(glfwCreateStandardCursor(GLFW_ARROW_CURSOR));
    bd->MouseCursors[ImGuiMouseCursor_ResizeNESW].Reset(glfwCreateStandardCursor(GLFW_ARROW_CURSOR));
    bd->MouseCursors[ImGuiMouseCursor_ResizeNWSE].Reset(glfwCreateStandardCursor(GLFW_ARROW_CURSOR));
    bd->MouseCursors[ImGuiMouseCursor_NotAllowed].Reset(glfwCreateStandardCursor(GLFW_ARROW_CURSOR));
#endif
    glfwSetErrorCallback(prev_error_callback);
#endif
#if GLFW_HAS_GETERROR && !defined(__EMSCRIPTEN__) // Eat errors (see #5908)
    (void)glfwGetError(nullptr);
#endif

    // Set platform dependent data in viewport
    ImGuiViewport* main_viewport  = ImGui::GetMainViewport();
    main_viewport->PlatformHandle = (void*)bd->Window;
#ifdef _WIN32
    main_viewport->PlatformHandleRaw = glfwGetWin32Window(bd->Window);
#elif defined(__APPLE__)
    main_viewport->PlatformHandleRaw = (void*)glfwGetCocoaWindow(bd->Window);
#else
    IM_UNUSED(main_viewport);
#endif

    // Emscripten: the same application can run on various platforms, so we detect the Apple platform at
    // runtime to override io.ConfigMacOSXBehaviors from its default (which is always false in Emscripten).
#ifdef __EMSCRIPTEN__
#if EMSCRIPTEN_USE_PORT_CONTRIB_GLFW3 >= 34020240817
    if (emscripten::glfw3::IsRuntimePlatformApple()) {
        io.ConfigMacOSXBehaviors = true;

        // Due to how the browser (poorly) handles the Meta Key, this line essentially disables repeats when
        // used. This means that Meta + V only registers a single key-press, even if the keys are held. This
        // is a compromise for dealing with this issue in ImGui since ImGui implements key repeat itself. See
        // the Emscripten GLFW usage documentation.
        emscripten::glfw3::SetSuperPlusKeyTimeouts(10, 10);
    }
#endif
#endif

    bd->ClientApi = client_api;
    (void)(*DataOwner)->Release();
    return {};
}

auto InitializeForVulkan(GLFWwindow* Window) -> std::expected<void, ErrorMessage> {
    return ImGui_ImplGlfw_Init(Window, GlfwClientApi_Vulkan);
}

auto Shutdown() -> void {
    ImGui_ImplGlfw_Data* bd = ImGui_ImplGlfw_GetBackendData();
    IM_ASSERT(bd != nullptr && "No platform backend to shutdown, or already shutdown?");

    ImGuiIO&           io          = ImGui::GetIO();
    ImGuiPlatformIO&   platform_io = ImGui::GetPlatformIO();
    ImGuiGlfwDataOwner Owner(bd);

#ifdef EMSCRIPTEN_USE_EMBEDDED_GLFW3
    if (bd->CanvasSelector)
        emscripten_set_wheel_callback(bd->CanvasSelector, nullptr, false, nullptr);
#endif
    io.BackendPlatformName     = nullptr;
    io.BackendPlatformUserData = nullptr;
    io.BackendFlags &=
        ~(ImGuiBackendFlags_HasMouseCursors | ImGuiBackendFlags_HasSetMousePos | ImGuiBackendFlags_HasGamepad);
    platform_io.ClearPlatformHandlers();
}

static auto ImGui_ImplGlfw_UpdateMouseData() -> void {
    ImGui_ImplGlfw_Data* bd = ImGui_ImplGlfw_GetBackendData();
    ImGuiIO&             io = ImGui::GetIO();

    // (those braces are here to reduce diff with multi-viewports support in 'docking' branch)
    {
        GLFWwindow* window = bd->Window;
#ifdef EMSCRIPTEN_USE_EMBEDDED_GLFW3
        const bool is_window_focused = true;
#else
        const bool is_window_focused = glfwGetWindowAttrib(window, GLFW_FOCUSED) != 0;
#endif
        if (is_window_focused) {
            // (Optional) Set OS mouse position from Dear ImGui if requested (rarely used, only when
            // io.ConfigNavMoveSetMousePos is enabled by user)
            if (io.WantSetMousePos)
                glfwSetCursorPos(window, (double)io.MousePos.x, (double)io.MousePos.y);
        }
    }
}

static auto ImGui_ImplGlfw_UpdateMouseCursor() -> void {
    ImGuiIO&             io = ImGui::GetIO();
    ImGui_ImplGlfw_Data* bd = ImGui_ImplGlfw_GetBackendData();
    if ((io.ConfigFlags & ImGuiConfigFlags_NoMouseCursorChange) ||
        glfwGetInputMode(bd->Window, GLFW_CURSOR) == GLFW_CURSOR_DISABLED) {
        bd->LastMouseCursor =
            nullptr; // Invalidate so that if user changes underlying cursor we will update it next time we can.
        return;
    }

    ImGuiMouseCursor imgui_cursor = ImGui::GetMouseCursor();
    // (those braces are here to reduce diff with multi-viewports support in 'docking' branch)
    {
        GLFWwindow* window = bd->Window;
        if (imgui_cursor == ImGuiMouseCursor_None || io.MouseDrawCursor) {
            if (bd->LastMouseCursor != nullptr) {
                // Hide OS mouse cursor if imgui is drawing it or if it wants no cursor
                glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_HIDDEN);
                bd->LastMouseCursor = nullptr;
            }
        } else {
            // Show OS mouse cursor FIXME-PLATFORM: Unfocused windows seems to fail changing the mouse cursor
            // with GLFW 3.2, but 3.3 works here.
#if GLFW_HAS_CREATECURSOR
            GLFWcursor* cursor = bd->MouseCursors[imgui_cursor].Get();
            if (cursor == nullptr)
                cursor = bd->MouseCursors[ImGuiMouseCursor_Arrow].Get();
            if (bd->LastMouseCursor != cursor) {
                glfwSetCursor(window, cursor);
                bd->LastMouseCursor = cursor;
            }
#endif
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        }
    }
}

// Update gamepad inputs
static inline auto Saturate(float v) -> float {
    return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
}
static auto ImGui_ImplGlfw_UpdateGamepads() -> void {
    ImGuiIO& io = ImGui::GetIO();
    if ((io.ConfigFlags & ImGuiConfigFlags_NavEnableGamepad) ==
        0) // FIXME: Technically feeding gamepad shouldn't depend on this now that they are regular inputs, but see #8075
        return;

    io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
#if GLFW_HAS_GAMEPAD_API && !defined(EMSCRIPTEN_USE_EMBEDDED_GLFW3)
    GLFWgamepadstate gamepad;
    if (!glfwGetGamepadState(GLFW_JOYSTICK_1, &gamepad))
        return;
#define MAP_BUTTON(KEY_NO, BUTTON_NO, _UNUSED)                                                                         \
    do {                                                                                                               \
        io.AddKeyEvent(KEY_NO, gamepad.buttons[BUTTON_NO] != 0);                                                       \
    } while (0)
#define MAP_ANALOG(KEY_NO, AXIS_NO, _UNUSED, V0, V1)                                                                   \
    do {                                                                                                               \
        float v = gamepad.axes[AXIS_NO];                                                                               \
        v       = (v - V0) / (V1 - V0);                                                                                \
        io.AddKeyAnalogEvent(KEY_NO, v > 0.10f, Saturate(v));                                                          \
    } while (0)
#else
    int                  axes_count = 0, buttons_count = 0;
    const float*         axes    = glfwGetJoystickAxes(GLFW_JOYSTICK_1, &axes_count);
    const unsigned char* buttons = glfwGetJoystickButtons(GLFW_JOYSTICK_1, &buttons_count);
    if (axes_count == 0 || buttons_count == 0)
        return;
#define MAP_BUTTON(KEY_NO, _UNUSED, BUTTON_NO)                                                                         \
    do {                                                                                                               \
        io.AddKeyEvent(KEY_NO, (buttons_count > BUTTON_NO && buttons[BUTTON_NO] == GLFW_PRESS));                       \
    } while (0)
#define MAP_ANALOG(KEY_NO, _UNUSED, AXIS_NO, V0, V1)                                                                   \
    do {                                                                                                               \
        float v = (axes_count > AXIS_NO) ? axes[AXIS_NO] : V0;                                                         \
        v       = (v - V0) / (V1 - V0);                                                                                \
        io.AddKeyAnalogEvent(KEY_NO, v > 0.10f, Saturate(v));                                                          \
    } while (0)
#endif
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    MAP_BUTTON(ImGuiKey_GamepadStart, GLFW_GAMEPAD_BUTTON_START, 7);
    MAP_BUTTON(ImGuiKey_GamepadBack, GLFW_GAMEPAD_BUTTON_BACK, 6);
    MAP_BUTTON(ImGuiKey_GamepadFaceLeft, GLFW_GAMEPAD_BUTTON_X, 2);  // Xbox X, PS Square
    MAP_BUTTON(ImGuiKey_GamepadFaceRight, GLFW_GAMEPAD_BUTTON_B, 1); // Xbox B, PS Circle
    MAP_BUTTON(ImGuiKey_GamepadFaceUp, GLFW_GAMEPAD_BUTTON_Y, 3);    // Xbox Y, PS Triangle
    MAP_BUTTON(ImGuiKey_GamepadFaceDown, GLFW_GAMEPAD_BUTTON_A, 0);  // Xbox A, PS Cross
    MAP_BUTTON(ImGuiKey_GamepadDpadLeft, GLFW_GAMEPAD_BUTTON_DPAD_LEFT, 13);
    MAP_BUTTON(ImGuiKey_GamepadDpadRight, GLFW_GAMEPAD_BUTTON_DPAD_RIGHT, 11);
    MAP_BUTTON(ImGuiKey_GamepadDpadUp, GLFW_GAMEPAD_BUTTON_DPAD_UP, 10);
    MAP_BUTTON(ImGuiKey_GamepadDpadDown, GLFW_GAMEPAD_BUTTON_DPAD_DOWN, 12);
    MAP_BUTTON(ImGuiKey_GamepadL1, GLFW_GAMEPAD_BUTTON_LEFT_BUMPER, 4);
    MAP_BUTTON(ImGuiKey_GamepadR1, GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER, 5);
    MAP_ANALOG(ImGuiKey_GamepadL2, GLFW_GAMEPAD_AXIS_LEFT_TRIGGER, 4, -0.75f, +1.0f);
    MAP_ANALOG(ImGuiKey_GamepadR2, GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER, 5, -0.75f, +1.0f);
    MAP_BUTTON(ImGuiKey_GamepadL3, GLFW_GAMEPAD_BUTTON_LEFT_THUMB, 8);
    MAP_BUTTON(ImGuiKey_GamepadR3, GLFW_GAMEPAD_BUTTON_RIGHT_THUMB, 9);
    MAP_ANALOG(ImGuiKey_GamepadLStickLeft, GLFW_GAMEPAD_AXIS_LEFT_X, 0, -0.25f, -1.0f);
    MAP_ANALOG(ImGuiKey_GamepadLStickRight, GLFW_GAMEPAD_AXIS_LEFT_X, 0, +0.25f, +1.0f);
    MAP_ANALOG(ImGuiKey_GamepadLStickUp, GLFW_GAMEPAD_AXIS_LEFT_Y, 1, -0.25f, -1.0f);
    MAP_ANALOG(ImGuiKey_GamepadLStickDown, GLFW_GAMEPAD_AXIS_LEFT_Y, 1, +0.25f, +1.0f);
    MAP_ANALOG(ImGuiKey_GamepadRStickLeft, GLFW_GAMEPAD_AXIS_RIGHT_X, 2, -0.25f, -1.0f);
    MAP_ANALOG(ImGuiKey_GamepadRStickRight, GLFW_GAMEPAD_AXIS_RIGHT_X, 2, +0.25f, +1.0f);
    MAP_ANALOG(ImGuiKey_GamepadRStickUp, GLFW_GAMEPAD_AXIS_RIGHT_Y, 3, -0.25f, -1.0f);
    MAP_ANALOG(ImGuiKey_GamepadRStickDown, GLFW_GAMEPAD_AXIS_RIGHT_Y, 3, +0.25f, +1.0f);
#undef MAP_BUTTON
#undef MAP_ANALOG
}

// For GFLW 3.2 + Windows: include a simplified non-monitor aware version of
// ImGui_ImplWin32_GetDpiScaleForMonitor(). This is merely a band-aid to make using GLFW 3.2 a little bit
// nicer, but prefer to use GLFW 3.3+ or the full correct functions from the Win32 backend.
#if !GLFW_HAS_PER_MONITOR_DPI && defined(_WIN32) && !defined(NOGDI)
static auto ImGui_ImplWin32_GetLegacyDpiScale() -> float {
    const HDC dc   = ::GetDC(nullptr);
    UINT      xdpi = ::GetDeviceCaps(dc, LOGPIXELSX);
    ::ReleaseDC(nullptr, dc);
    return (float)xdpi / 96.0f;
}
static auto glfwGetWindowContentScale(GLFWwindow*, float* x_scale, float* y_scale) -> void {
    *x_scale = *y_scale = ImGui_ImplWin32_GetLegacyDpiScale();
}
static auto glfwGetMonitorContentScale(GLFWmonitor*, float* x_scale, float* y_scale) -> void {
    *x_scale = *y_scale = ImGui_ImplWin32_GetLegacyDpiScale();
}
#undef GLFW_HAS_PER_MONITOR_DPI
#define GLFW_HAS_PER_MONITOR_DPI 1
#endif

// - On Windows the process needs to be marked DPI-aware!! SDL2 doesn't do it by default. You can call
// ::SetProcessDPIAware() or call ImGui_ImplWin32_EnableDpiAwareness() from Win32 backend.
// - Apple platforms use FramebufferScale so we always return 1.0f.
// - Some accessibility applications are declaring virtual monitors with a DPI of 0.0f, see #7902. We preserve
// this value for caller to handle.
auto ImGui_ImplGlfw_GetContentScaleForWindow(GLFWwindow* window) -> float {
#if GLFW_HAS_WAYLAND
    if (ImGui_ImplGlfw_Data* bd = ImGui_ImplGlfw_GetBackendData())
        if (bd->IsWayland)
            return 1.0f;
#endif
#if GLFW_HAS_PER_MONITOR_DPI && !(defined(__APPLE__) || defined(__EMSCRIPTEN__) || defined(__ANDROID__))
    float x_scale, y_scale;
    glfwGetWindowContentScale(window, &x_scale, &y_scale);
    return x_scale;
#else
    IM_UNUSED(window);
    return 1.0f;
#endif
}

auto ImGui_ImplGlfw_GetContentScaleForMonitor(GLFWmonitor* monitor) -> float {
#if GLFW_HAS_WAYLAND
    if (ImGui_ImplGlfw_IsWayland()) // We can't access our bd->IsWayland cache for a monitor.
        return 1.0f;
#endif
#if GLFW_HAS_PER_MONITOR_DPI && !(defined(__APPLE__) || defined(__EMSCRIPTEN__) || defined(__ANDROID__))
    float x_scale, y_scale;
    glfwGetMonitorContentScale(monitor, &x_scale, &y_scale);
    return x_scale;
#else
    IM_UNUSED(monitor);
    return 1.0f;
#endif
}

static auto ImGui_ImplGlfw_GetWindowSizeAndFramebufferScale(GLFWwindow* window,
                                                            ImVec2*     out_size,
                                                            ImVec2*     out_framebuffer_scale) -> void {
    int w, h;
    int display_w, display_h;
    glfwGetWindowSize(window, &w, &h);
    glfwGetFramebufferSize(window, &display_w, &display_h);
    float fb_scale_x = (w > 0) ? (float)display_w / (float)w : 1.0f;
    float fb_scale_y = (h > 0) ? (float)display_h / (float)h : 1.0f;
#if GLFW_HAS_WAYLAND
    ImGui_ImplGlfw_Data* bd = ImGui_ImplGlfw_GetBackendData();
    if (!bd->IsWayland)
        fb_scale_x = fb_scale_y = 1.0f;
#endif
    if (out_size != nullptr)
        *out_size = ImVec2((float)w, (float)h);
    if (out_framebuffer_scale != nullptr)
        *out_framebuffer_scale = ImVec2(fb_scale_x, fb_scale_y);
}

auto NewFrame() -> void {
    ImGuiIO&             io = ImGui::GetIO();
    ImGui_ImplGlfw_Data* bd = ImGui_ImplGlfw_GetBackendData();
    IM_ASSERT(bd != nullptr && "Context or backend not initialized! Did you call ImGui_ImplGlfw_InitForXXX()?");

    // Setup main viewport size (every frame to accommodate for window resizing)
    ImGui_ImplGlfw_GetWindowSizeAndFramebufferScale(bd->Window, &io.DisplaySize, &io.DisplayFramebufferScale);

    // Setup time step (Accept glfwGetTime() not returning a monotonically increasing value. Seems to happens
    // on disconnecting peripherals and probably on VMs and Emscripten, see #6491, #6189, #6114, #3644)
    double current_time = glfwGetTime();
    if (current_time <= bd->Time)
        current_time = bd->Time + 0.00001;
    io.DeltaTime = bd->Time > 0.0 ? (float)(current_time - bd->Time) : (float)(1.0f / 60.0f);
    bd->Time     = current_time;

    ImGui_ImplGlfw_UpdateMouseData();
    ImGui_ImplGlfw_UpdateMouseCursor();

    // Update game controllers (if enabled and available)
    ImGui_ImplGlfw_UpdateGamepads();
}

// GLFW doesn't provide a portable sleep function
auto ImGui_ImplGlfw_Sleep(int milliseconds) -> void {
#ifdef _WIN32
    ::Sleep(milliseconds);
#else
    usleep(milliseconds * 1000);
#endif
}

#ifdef EMSCRIPTEN_USE_EMBEDDED_GLFW3
static auto ImGui_ImplGlfw_OnCanvasSizeChange(int event_type, const EmscriptenUiEvent* event, void* user_data)
    -> EM_BOOL {
    ImGui_ImplGlfw_Data* bd = (ImGui_ImplGlfw_Data*)user_data;
    double               canvas_width, canvas_height;
    emscripten_get_element_css_size(bd->CanvasSelector, &canvas_width, &canvas_height);
    glfwSetWindowSize(bd->Window, (int)canvas_width, (int)canvas_height);
    return true;
}

static auto ImGui_ImplEmscripten_FullscreenChangeCallback(int                                    event_type,
                                                          const EmscriptenFullscreenChangeEvent* event,
                                                          void*                                  user_data) -> EM_BOOL {
    ImGui_ImplGlfw_Data* bd = (ImGui_ImplGlfw_Data*)user_data;
    double               canvas_width, canvas_height;
    emscripten_get_element_css_size(bd->CanvasSelector, &canvas_width, &canvas_height);
    glfwSetWindowSize(bd->Window, (int)canvas_width, (int)canvas_height);
    return true;
}

// 'canvas_selector' is a CSS selector. The event listener is applied to the first element that matches the
// query. STRING MUST PERSIST FOR THE APPLICATION DURATION. PLEASE USE A STRING LITERAL OR ENSURE POINTER WILL
// STAY VALID.
auto ImGui_ImplGlfw_InstallEmscriptenCallbacks(GLFWwindow*, const char* canvas_selector) -> void {
    IM_ASSERT(canvas_selector != nullptr);
    ImGui_ImplGlfw_Data* bd = ImGui_ImplGlfw_GetBackendData();
    IM_ASSERT(bd != nullptr && "Context or backend not initialized! Did you call ImGui_ImplGlfw_InitForXXX()?");

    bd->CanvasSelector = canvas_selector;
    emscripten_set_resize_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, bd, false, ImGui_ImplGlfw_OnCanvasSizeChange);
    emscripten_set_fullscreenchange_callback(
        EMSCRIPTEN_EVENT_TARGET_DOCUMENT, bd, false, ImGui_ImplEmscripten_FullscreenChangeCallback);

    // Change the size of the GLFW window according to the size of the canvas
    ImGui_ImplGlfw_OnCanvasSizeChange(EMSCRIPTEN_EVENT_RESIZE, {}, bd);

    // Register Emscripten Wheel callback to workaround issue in Emscripten GLFW Emulation (#6096) We
    // intentionally do not check 'if (install_callbacks)' here, as some users may set it to false and call
    // GLFW callback themselves. FIXME: May break chaining in case user registered their own Emscripten
    // callback?
    emscripten_set_wheel_callback(bd->CanvasSelector, bd, false, ImGui_ImplEmscripten_WheelCallback);
}
#elif defined(EMSCRIPTEN_USE_PORT_CONTRIB_GLFW3)
// When using --use-port=contrib.glfw3 for the GLFW implementation, you can override the behavior of this call
// by invoking emscripten_glfw_make_canvas_resizable afterward. See the Emscripten GLFW canvas
// documentation. for an explanation
auto ImGui_ImplGlfw_InstallEmscriptenCallbacks(GLFWwindow* window, const char* canvas_selector) -> void {
    GLFWwindow* w = (GLFWwindow*)(EM_ASM_INT({ return Module.glfwGetWindow(UTF8ToString($0)); }, canvas_selector));
    IM_ASSERT(window == w); // Sanity check
    IM_UNUSED(w);
    emscripten_glfw_make_canvas_resizable(window, "window", nullptr);
}
#endif // #ifdef EMSCRIPTEN_USE_PORT_CONTRIB_GLFW3

// -----------------------------------------------------------------------------

auto AddModifiers(ImGuiIO& IO, const InputModifiers& Modifiers) -> void {
    IO.AddKeyEvent(ImGuiMod_Shift, Modifiers.IsShift);
    IO.AddKeyEvent(ImGuiMod_Ctrl, Modifiers.IsControl);
    IO.AddKeyEvent(ImGuiMod_Alt, Modifiers.IsAlt);
    IO.AddKeyEvent(ImGuiMod_Super, Modifiers.IsSuper);
}

auto ToImGuiMouseButton(WindowMouseButton Button) -> int {
    switch (Button) {
    case WindowMouseButton::Left:
        return 0;
    case WindowMouseButton::Right:
        return 1;
    case WindowMouseButton::Middle:
        return 2;
    case WindowMouseButton::X1:
        return 3;
    case WindowMouseButton::X2:
        return 4;
    default:
        return -1;
    }
}

auto ApplyInput(const InputFrame& Frame) -> void {
    if (!ImGui::GetCurrentContext())
        return;
    ImGuiIO& IO = ImGui::GetIO();
    for (const InputEvent& Event : Frame.Events) {
        std::visit(
            [&]<class T>(const T& Value) -> void {
                if constexpr (std::is_same_v<T, PointerInputEvent>) {
                    IO.AddMousePosEvent(Value.Position.X, Value.Position.Y);
                } else if constexpr (std::is_same_v<T, MouseButtonInputEvent>) {
                    AddModifiers(IO, Value.Modifiers);
                    const int Button = ToImGuiMouseButton(Value.Button);
                    if (Button >= 0)
                        IO.AddMouseButtonEvent(Button, Value.Action == InputAction::Press);
                } else if constexpr (std::is_same_v<T, WheelInputEvent>) {
                    IO.AddMouseWheelEvent(Value.X, Value.Y);
                } else if constexpr (std::is_same_v<T, KeyInputEvent>) {
                    AddModifiers(IO, Value.Modifiers);
                    if (Value.Action == InputAction::Repeat)
                        return;
                    const ImGuiKey Key = ImGui_ImplGlfw_KeyToImGuiKey(Value.NativeKey, Value.Scancode);
                    if (Key == ImGuiKey_None)
                        return;
                    IO.AddKeyEvent(Key, Value.Action == InputAction::Press);
                    IO.SetKeyEventNativeData(Key, Value.NativeKey, Value.Scancode);
                } else if constexpr (std::is_same_v<T, TextInputEvent>) {
                    IO.AddInputCharacter(Value.Codepoint);
                } else if constexpr (std::is_same_v<T, FocusInputEvent>) {
                    IO.AddFocusEvent(Value.IsFocused);
                } else if constexpr (std::is_same_v<T, CursorEnterInputEvent>) {
                    if (Value.IsInside)
                        IO.AddMousePosEvent(Frame.Mouse.CurrentCursorPosition.X, Frame.Mouse.CurrentCursorPosition.Y);
                    else
                        IO.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
                } else if constexpr (std::is_same_v<T, ResetInputEvent>) {
                    IO.ClearInputKeys();
                    IO.ClearInputMouse();
                    IO.AddMousePosEvent(Frame.Mouse.CurrentCursorPosition.X, Frame.Mouse.CurrentCursorPosition.Y);
                }
            },
            Event);
    }
}

auto ResetInput() -> void {
    if (!ImGui::GetCurrentContext())
        return;
    ImGuiIO& IO = ImGui::GetIO();
    IO.ClearInputKeys();
    IO.ClearInputMouse();
    IO.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    IO.AddFocusEvent(false);
}

} // namespace ImGuiGlfwAdapter
} // namespace SoulEngine

#if defined(__clang__)
#pragma clang diagnostic pop
#endif
