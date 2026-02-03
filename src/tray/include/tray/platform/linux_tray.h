#pragma once

#if defined(__linux__) && !defined(__ANDROID__)

#include "tray_interface.h"
#include <atomic>
#include <map>

// Forward declarations for GLib/GTK types
typedef struct _GtkStatusIcon GtkStatusIcon;
typedef struct _GtkWidget GtkWidget;
typedef struct _AppIndicator AppIndicator;

namespace tray {

class LinuxTray : public TrayInterface {
public:
    LinuxTray();
    ~LinuxTray() override;

    // TrayInterface implementation
    bool initialize(const std::string& app_name, const std::string& icon_path) override;
    void run() override;
    void stop() override;
    void set_menu(const Menu& menu) override;
    void update_menu() override;
    void show_notification(
        const std::string& title,
        const std::string& message,
        NotificationType type = NotificationType::INFO
    ) override;
    void set_icon(const std::string& icon_path) override;
    void set_tooltip(const std::string& tooltip) override;
    void set_ready_callback(std::function<void()> callback) override;
    void set_log_level(const std::string& log_level) override;
    void set_menu_update_callback(std::function<void()> callback) override;

private:
    void build_gtk_menu();
    static void on_menu_item_activate(GtkWidget* widget, void* user_data);

    std::string app_name_;
    std::string icon_path_;
    std::string log_level_;
    std::function<void()> ready_callback_;
    std::function<void()> menu_update_callback_;
    std::atomic<bool> should_exit_;

    // GTK/AppIndicator objects
    AppIndicator* indicator_;
    GtkWidget* gtk_menu_;
    Menu current_menu_;

    // Store callbacks with menu item IDs
    std::map<int, MenuCallback> menu_callbacks_;
    int next_menu_id_;
};

} // namespace tray

#endif // __linux__ && !__ANDROID__
