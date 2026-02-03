#ifdef _WIN32

#include "tray/platform/windows_tray.h"
#include <iostream>
#include <codecvt>
#include <locale>

// Undefine Windows macros that conflict with our enums
#ifdef ERROR
#undef ERROR
#endif

// Helper macro for debug logging
#define DEBUG_LOG_TRAY(tray, msg) \
    if ((tray)->is_debug()) { \
        std::cout << "DEBUG: " << msg << std::endl; \
    }

namespace tray {

namespace {
    // Helper function to convert UTF-8 string to wide string
    std::wstring utf8_to_wstring(const std::string& str) {
        if (str.empty()) return std::wstring();
        int size_needed = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
        std::wstring result(size_needed, 0);
        MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &result[0], size_needed);
        if (!result.empty() && result.back() == L'\0') {
            result.pop_back();
        }
        return result;
    }
}

WindowsTray::WindowsTray()
    : hwnd_(nullptr)
    , hinst_(GetModuleHandle(nullptr))
    , hmenu_(nullptr)
    , notification_icon_(nullptr)
    , should_exit_(false)
    , next_menu_id_(MENU_ID_START)
    , log_level_("info")
{
    ZeroMemory(&nid_, sizeof(nid_));
}

WindowsTray::~WindowsTray() {
    remove_tray_icon();
    if (hmenu_) {
        DestroyMenu(hmenu_);
    }
    if (hwnd_) {
        DestroyWindow(hwnd_);
    }
}

bool WindowsTray::initialize(const std::string& app_name, const std::string& icon_path) {
    DEBUG_LOG_TRAY(this, "WindowsTray::initialize() called");
    app_name_ = app_name;
    icon_path_ = icon_path;
    tooltip_ = app_name;

    if (!register_window_class()) {
        std::cerr << "Failed to register window class" << std::endl;
        return false;
    }

    if (!create_window()) {
        std::cerr << "Failed to create window" << std::endl;
        return false;
    }

    if (!add_tray_icon()) {
        std::cerr << "Failed to add tray icon" << std::endl;
        return false;
    }

    if (ready_callback_) {
        ready_callback_();
    }

    return true;
}

bool WindowsTray::register_window_class() {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = window_proc_static;
    wc.hInstance = hinst_;
    wc.lpszClassName = L"LemonadeTrayAppClass";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);

    if (!RegisterClassExW(&wc)) {
        DWORD error = GetLastError();
        if (error != ERROR_CLASS_ALREADY_EXISTS) {
            return false;
        }
    }

    return true;
}

bool WindowsTray::create_window() {
    hwnd_ = CreateWindowExW(
        0,
        L"LemonadeTrayAppClass",
        L"Lemonade Tray",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        CW_USEDEFAULT, CW_USEDEFAULT,
        nullptr,
        nullptr,
        hinst_,
        this
    );

    if (!hwnd_) {
        return false;
    }

    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    return true;
}

bool WindowsTray::add_tray_icon() {
    nid_.cbSize = sizeof(NOTIFYICONDATAW);
    nid_.hWnd = hwnd_;
    nid_.uID = 1;
    nid_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid_.uCallbackMessage = WM_TRAYICON;

    nid_.hIcon = (HICON)LoadImageA(
        nullptr,
        icon_path_.c_str(),
        IMAGE_ICON,
        0, 0,
        LR_LOADFROMFILE | LR_DEFAULTSIZE | LR_SHARED
    );

    if (!nid_.hIcon) {
        nid_.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    }

    notification_icon_ = nid_.hIcon;

    std::wstring tooltip_wide = utf8_to_wstring(tooltip_);
    wcsncpy_s(nid_.szTip, tooltip_wide.c_str(), _TRUNCATE);

    if (!Shell_NotifyIconW(NIM_ADD, &nid_)) {
        return false;
    }

    nid_.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid_);

    return true;
}

void WindowsTray::remove_tray_icon() {
    if (hwnd_) {
        Shell_NotifyIconW(NIM_DELETE, &nid_);
    }
}

void WindowsTray::run() {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0 && !should_exit_) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

void WindowsTray::stop() {
    should_exit_ = true;
    if (hwnd_) {
        PostMessageW(hwnd_, WM_QUIT, 0, 0);
    }
}

void WindowsTray::set_menu(const Menu& menu) {
    current_menu_ = menu;

    if (hmenu_) {
        DestroyMenu(hmenu_);
        hmenu_ = nullptr;
    }

    hmenu_ = create_popup_menu(current_menu_);
}

void WindowsTray::update_menu() {
    if (menu_update_callback_) {
        menu_update_callback_();
    }
    set_menu(current_menu_);
}

HMENU WindowsTray::create_popup_menu(const Menu& menu) {
    HMENU hmenu = CreatePopupMenu();
    menu_callbacks_.clear();
    next_menu_id_ = MENU_ID_START;

    add_menu_items(hmenu, menu.items);

    return hmenu;
}

void WindowsTray::add_menu_items(HMENU hmenu, const std::vector<MenuItem>& items) {
    for (const auto& item : items) {
        if (item.is_separator) {
            AppendMenuW(hmenu, MF_SEPARATOR, 0, nullptr);
        } else if (item.submenu) {
            HMENU hsubmenu = CreatePopupMenu();
            add_menu_items(hsubmenu, item.submenu->items);

            std::wstring text_wide = utf8_to_wstring(item.text);
            UINT flags = MF_POPUP;
            if (!item.enabled) flags |= MF_GRAYED;

            AppendMenuW(hmenu, flags, reinterpret_cast<UINT_PTR>(hsubmenu), text_wide.c_str());
        } else {
            int menu_id = next_menu_id_++;
            std::wstring text_wide = utf8_to_wstring(item.text);

            UINT flags = MF_STRING;
            if (!item.enabled) flags |= MF_GRAYED;
            if (item.checked) flags |= MF_CHECKED;

            AppendMenuW(hmenu, flags, menu_id, text_wide.c_str());

            if (item.callback) {
                menu_callbacks_[menu_id] = item.callback;
            }
        }
    }
}

void WindowsTray::show_context_menu() {
    if (!hmenu_) return;

    POINT cursor_pos;
    GetCursorPos(&cursor_pos);

    SetForegroundWindow(hwnd_);

    TrackPopupMenu(
        hmenu_,
        TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_RIGHTALIGN,
        cursor_pos.x,
        cursor_pos.y,
        0,
        hwnd_,
        nullptr
    );

    PostMessageW(hwnd_, WM_NULL, 0, 0);
}

void WindowsTray::show_notification(
    const std::string& title,
    const std::string& message,
    NotificationType type)
{
    last_notification_title_ = title;

    nid_.uFlags = NIF_INFO;

    std::wstring title_wide = utf8_to_wstring(title);
    std::wstring message_wide = utf8_to_wstring(message);

    wcsncpy_s(nid_.szInfoTitle, title_wide.c_str(), _TRUNCATE);
    wcsncpy_s(nid_.szInfo, message_wide.c_str(), _TRUNCATE);

    nid_.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON;
    nid_.hBalloonIcon = notification_icon_;

    Shell_NotifyIconW(NIM_MODIFY, &nid_);

    nid_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
}

void WindowsTray::set_icon(const std::string& icon_path) {
    icon_path_ = icon_path;

    HICON hicon = (HICON)LoadImageA(
        nullptr,
        icon_path.c_str(),
        IMAGE_ICON,
        0, 0,
        LR_LOADFROMFILE | LR_DEFAULTSIZE
    );

    if (hicon) {
        nid_.hIcon = hicon;
        Shell_NotifyIconW(NIM_MODIFY, &nid_);
    }
}

void WindowsTray::set_tooltip(const std::string& tooltip) {
    tooltip_ = tooltip;
    std::wstring tooltip_wide = utf8_to_wstring(tooltip);
    wcsncpy_s(nid_.szTip, tooltip_wide.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid_);
}

void WindowsTray::set_log_level(const std::string& log_level) {
    log_level_ = log_level;
}

void WindowsTray::set_ready_callback(std::function<void()> callback) {
    ready_callback_ = callback;
}

void WindowsTray::set_menu_update_callback(std::function<void()> callback) {
    menu_update_callback_ = callback;
}

LRESULT CALLBACK WindowsTray::window_proc_static(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    WindowsTray* tray = nullptr;

    if (msg == WM_CREATE) {
        CREATESTRUCT* cs = reinterpret_cast<CREATESTRUCT*>(lparam);
        tray = reinterpret_cast<WindowsTray*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(tray));
    } else {
        tray = reinterpret_cast<WindowsTray*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (tray) {
        return tray->window_proc(hwnd, msg, wparam, lparam);
    }

    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

LRESULT WindowsTray::window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        case WM_TRAYICON:
            on_tray_icon(lparam);
            return 0;

        case WM_COMMAND:
            on_command(wparam);
            return 0;

        case WM_DESTROY:
            on_destroy();
            return 0;

        default:
            return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
}

void WindowsTray::on_tray_icon(LPARAM lparam) {
    UINT msg = LOWORD(lparam);

    switch (msg) {
        case WM_RBUTTONUP:
        case WM_LBUTTONUP:
            if (menu_update_callback_) {
                menu_update_callback_();
            }
            show_context_menu();
            break;
    }
}

void WindowsTray::on_command(WPARAM wparam) {
    int menu_id = LOWORD(wparam);

    auto it = menu_callbacks_.find(menu_id);
    if (it != menu_callbacks_.end() && it->second) {
        it->second();
    }
}

void WindowsTray::on_destroy() {
    PostQuitMessage(0);
}

} // namespace tray

#endif // _WIN32
