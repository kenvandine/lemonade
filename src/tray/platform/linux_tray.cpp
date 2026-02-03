#if defined(__linux__) && !defined(__ANDROID__)

#include "tray/platform/linux_tray.h"
#include <iostream>
#include <gtk/gtk.h>

// Support both ayatana-appindicator and original libappindicator
#if __has_include(<libayatana-appindicator/app-indicator.h>)
#include <libayatana-appindicator/app-indicator.h>
#else
#include <libappindicator/app-indicator.h>
#endif

#include <libnotify/notify.h>

namespace tray {

// Helper struct to pass callback data to GTK menu items
struct MenuCallbackData {
    LinuxTray* tray;
    int menu_id;
};

LinuxTray::LinuxTray()
    : should_exit_(false)
    , log_level_("info")
    , indicator_(nullptr)
    , gtk_menu_(nullptr)
    , next_menu_id_(1000)
{
}

LinuxTray::~LinuxTray() {
    if (indicator_) {
        g_object_unref(indicator_);
        indicator_ = nullptr;
    }
    if (gtk_menu_) {
        gtk_widget_destroy(gtk_menu_);
        gtk_menu_ = nullptr;
    }
    notify_uninit();
}

bool LinuxTray::initialize(const std::string& app_name, const std::string& icon_path) {
    app_name_ = app_name;
    icon_path_ = icon_path;

    // Initialize GTK
    if (!gtk_init_check(nullptr, nullptr)) {
        std::cerr << "[Linux Tray] Failed to initialize GTK" << std::endl;
        return false;
    }

    // Initialize libnotify
    if (!notify_init(app_name.c_str())) {
        std::cerr << "[Linux Tray] Failed to initialize libnotify" << std::endl;
        // Continue anyway, notifications just won't work
    }

    // Create AppIndicator
    indicator_ = app_indicator_new(
        "lemonade-server-tray",
        icon_path_.c_str(),
        APP_INDICATOR_CATEGORY_APPLICATION_STATUS
    );

    if (!indicator_) {
        std::cerr << "[Linux Tray] Failed to create AppIndicator" << std::endl;
        return false;
    }

    app_indicator_set_status(indicator_, APP_INDICATOR_STATUS_ACTIVE);
    app_indicator_set_title(indicator_, app_name_.c_str());

    // Create initial empty menu (required for AppIndicator)
    gtk_menu_ = gtk_menu_new();
    app_indicator_set_menu(indicator_, GTK_MENU(gtk_menu_));

    if (log_level_ == "debug") {
        std::cout << "[Linux Tray] Initialized successfully" << std::endl;
    }

    // Call ready callback
    if (ready_callback_) {
        ready_callback_();
    }

    return true;
}

void LinuxTray::run() {
    if (log_level_ == "debug") {
        std::cout << "[Linux Tray] Starting GTK main loop" << std::endl;
    }
    gtk_main();
}

void LinuxTray::stop() {
    if (log_level_ == "debug") {
        std::cout << "[Linux Tray] Stopping GTK main loop" << std::endl;
    }
    should_exit_ = true;
    gtk_main_quit();
}

void LinuxTray::on_menu_item_activate(GtkWidget* widget, void* user_data) {
    MenuCallbackData* data = static_cast<MenuCallbackData*>(user_data);
    if (data && data->tray) {
        auto it = data->tray->menu_callbacks_.find(data->menu_id);
        if (it != data->tray->menu_callbacks_.end() && it->second) {
            it->second();
        }
    }
}

void LinuxTray::build_gtk_menu() {
    // Destroy old menu
    if (gtk_menu_) {
        gtk_widget_destroy(gtk_menu_);
    }

    // Create new menu
    gtk_menu_ = gtk_menu_new();
    menu_callbacks_.clear();
    next_menu_id_ = 1000;

    // Add menu items
    for (const auto& item : current_menu_.items) {
        GtkWidget* menu_item;

        if (item.is_separator) {
            menu_item = gtk_separator_menu_item_new();
        } else if (item.submenu) {
            // Create submenu
            menu_item = gtk_menu_item_new_with_label(item.text.c_str());
            GtkWidget* submenu = gtk_menu_new();

            for (const auto& subitem : item.submenu->items) {
                GtkWidget* sub_menu_item;
                if (subitem.is_separator) {
                    sub_menu_item = gtk_separator_menu_item_new();
                } else {
                    sub_menu_item = gtk_menu_item_new_with_label(subitem.text.c_str());
                    gtk_widget_set_sensitive(sub_menu_item, subitem.enabled);

                    if (subitem.callback) {
                        int menu_id = next_menu_id_++;
                        menu_callbacks_[menu_id] = subitem.callback;

                        MenuCallbackData* data = new MenuCallbackData{this, menu_id};
                        g_signal_connect_data(
                            sub_menu_item, "activate",
                            G_CALLBACK(on_menu_item_activate), data,
                            [](gpointer data, GClosure*) { delete static_cast<MenuCallbackData*>(data); },
                            G_CONNECT_DEFAULT
                        );
                    }
                }
                gtk_menu_shell_append(GTK_MENU_SHELL(submenu), sub_menu_item);
            }

            gtk_menu_item_set_submenu(GTK_MENU_ITEM(menu_item), submenu);
            gtk_widget_set_sensitive(menu_item, item.enabled);
        } else {
            // Regular menu item
            if (item.checked) {
                menu_item = gtk_check_menu_item_new_with_label(item.text.c_str());
                gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(menu_item), TRUE);
            } else {
                menu_item = gtk_menu_item_new_with_label(item.text.c_str());
            }
            gtk_widget_set_sensitive(menu_item, item.enabled);

            if (item.callback) {
                int menu_id = next_menu_id_++;
                menu_callbacks_[menu_id] = item.callback;

                MenuCallbackData* data = new MenuCallbackData{this, menu_id};
                g_signal_connect_data(
                    menu_item, "activate",
                    G_CALLBACK(on_menu_item_activate), data,
                    [](gpointer data, GClosure*) { delete static_cast<MenuCallbackData*>(data); },
                    G_CONNECT_DEFAULT
                );
            }
        }

        gtk_menu_shell_append(GTK_MENU_SHELL(gtk_menu_), menu_item);
    }

    gtk_widget_show_all(gtk_menu_);
    app_indicator_set_menu(indicator_, GTK_MENU(gtk_menu_));
}

void LinuxTray::set_menu(const Menu& menu) {
    current_menu_ = menu;
    build_gtk_menu();
}

void LinuxTray::update_menu() {
    // Trigger menu update callback if set
    if (menu_update_callback_) {
        menu_update_callback_();
    }
    build_gtk_menu();
}

void LinuxTray::show_notification(
    const std::string& title,
    const std::string& message,
    NotificationType type)
{
    NotifyNotification* notification = notify_notification_new(
        title.c_str(),
        message.c_str(),
        icon_path_.c_str()
    );

    if (notification) {
        // Set urgency based on type
        NotifyUrgency urgency = NOTIFY_URGENCY_NORMAL;
        switch (type) {
            case NotificationType::ERROR:
                urgency = NOTIFY_URGENCY_CRITICAL;
                break;
            case NotificationType::WARNING:
                urgency = NOTIFY_URGENCY_NORMAL;
                break;
            default:
                urgency = NOTIFY_URGENCY_LOW;
                break;
        }
        notify_notification_set_urgency(notification, urgency);

        GError* error = nullptr;
        if (!notify_notification_show(notification, &error)) {
            if (error) {
                std::cerr << "[Linux Tray] Failed to show notification: " << error->message << std::endl;
                g_error_free(error);
            }
        }
        g_object_unref(notification);
    }
}

void LinuxTray::set_icon(const std::string& icon_path) {
    icon_path_ = icon_path;
    if (indicator_) {
        app_indicator_set_icon(indicator_, icon_path_.c_str());
    }
}

void LinuxTray::set_tooltip(const std::string& tooltip) {
    if (indicator_) {
        app_indicator_set_title(indicator_, tooltip.c_str());
    }
}

void LinuxTray::set_ready_callback(std::function<void()> callback) {
    ready_callback_ = callback;
}

void LinuxTray::set_log_level(const std::string& log_level) {
    log_level_ = log_level;
}

void LinuxTray::set_menu_update_callback(std::function<void()> callback) {
    menu_update_callback_ = callback;
}

} // namespace tray

#endif // __linux__ && !__ANDROID__
