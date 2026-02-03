#pragma once

#include "tray/platform/tray_interface.h"
#include "tray/server_client.h"
#include <memory>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/types.h>
#endif

namespace tray {

class TrayApp {
public:
    TrayApp(const std::string& host = "127.0.0.1", int port = 8000);
    ~TrayApp();

    // Configuration
    void set_port(int port);
    void set_host(const std::string& host);
    void set_log_level(const std::string& level);
    void set_icon_path(const std::string& path);

    // Run the tray application
    int run();

    // Shutdown
    void shutdown();

private:
    // Menu building
    void build_menu();
    Menu create_menu();

    // Menu actions
    void on_start_server();
    void on_stop_server();
    void on_load_model(const std::string& model_name);
    void on_unload_model(const std::string& model_name);
    void on_open_documentation();
    void on_quit();

    // Helpers
    void open_url(const std::string& url);
    void show_notification(const std::string& title, const std::string& message);
    bool find_server_binary();
    bool start_server_process();
    void stop_server_process();
    bool is_server_running();

    // Status monitoring
    void start_status_monitor();
    void stop_status_monitor();
    void update_status();

    // Member variables
    std::unique_ptr<TrayInterface> tray_;
    std::unique_ptr<ServerClient> client_;

    std::string host_;
    int port_;
    std::string log_level_;
    std::string icon_path_;
    std::string server_binary_;

    std::atomic<bool> should_exit_;
    std::atomic<bool> server_running_;

    // Status monitor thread
    std::thread status_thread_;
    std::atomic<bool> stop_status_monitor_;

    // Server process tracking
#ifdef _WIN32
    HANDLE server_process_;
#else
    pid_t server_pid_;
#endif
};

} // namespace tray
