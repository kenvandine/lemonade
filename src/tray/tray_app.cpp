#include "tray/tray_app.h"
#include <iostream>
#include <filesystem>
#include <chrono>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <fcntl.h>
#endif

namespace fs = std::filesystem;

namespace tray {

TrayApp::TrayApp(const std::string& host, int port)
    : host_(host)
    , port_(port)
    , log_level_("info")
    , should_exit_(false)
    , server_running_(false)
    , stop_status_monitor_(false)
#ifdef _WIN32
    , server_process_(nullptr)
#else
    , server_pid_(0)
#endif
{
    client_ = std::make_unique<ServerClient>(host_, port_);
}

TrayApp::~TrayApp() {
    shutdown();
}

void TrayApp::set_port(int port) {
    port_ = port;
    if (client_) {
        client_->set_port(port);
    }
}

void TrayApp::set_host(const std::string& host) {
    host_ = host;
    if (client_) {
        client_->set_host(host);
    }
}

void TrayApp::set_log_level(const std::string& level) {
    log_level_ = level;
}

void TrayApp::set_icon_path(const std::string& path) {
    icon_path_ = path;
}

int TrayApp::run() {
    // Find server binary for potential launch
    find_server_binary();

    // Create tray
    tray_ = create_tray();
    if (!tray_) {
        std::cerr << "Error: Failed to create tray for this platform" << std::endl;
        return 1;
    }

    // Set log level
    tray_->set_log_level(log_level_);

    // Set ready callback
    tray_->set_ready_callback([this]() {
        // Check if server is already running
        update_status();
        if (server_running_) {
            show_notification("Connected", "Connected to Lemonade Server on port " + std::to_string(port_));
        } else {
            show_notification("Server Not Running", "Lemonade Server is not running. Click the tray icon to start it.");
        }
    });

    // Set menu update callback
    tray_->set_menu_update_callback([this]() {
        update_status();
        build_menu();
    });

    // Find icon path if not set
    if (icon_path_.empty()) {
        // Try common locations
        std::vector<std::string> icon_paths = {
            "resources/static/favicon.ico",
            "/opt/share/lemonade-server/resources/static/favicon.ico",
            "/usr/share/lemonade-server/resources/static/favicon.ico"
        };

#ifdef _WIN32
        char exe_path[MAX_PATH];
        if (GetModuleFileNameA(NULL, exe_path, MAX_PATH)) {
            fs::path exe_dir = fs::path(exe_path).parent_path();
            icon_paths.insert(icon_paths.begin(), (exe_dir / "resources" / "static" / "favicon.ico").string());
        }
#else
        char exe_path[1024];
        ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
        if (len != -1) {
            exe_path[len] = '\0';
            fs::path exe_dir = fs::path(exe_path).parent_path();
            icon_paths.insert(icon_paths.begin(), (exe_dir / "resources" / "static" / "favicon.ico").string());
        }
#endif

        for (const auto& path : icon_paths) {
            if (fs::exists(path)) {
                icon_path_ = path;
                break;
            }
        }
    }

    if (log_level_ == "debug") {
        std::cout << "[TrayApp] Using icon: " << icon_path_ << std::endl;
    }

    // Initialize tray
    if (!tray_->initialize("Lemonade Server", icon_path_)) {
        std::cerr << "Error: Failed to initialize tray" << std::endl;
        return 1;
    }

    // Build initial menu
    update_status();
    build_menu();

    // Start status monitor
    start_status_monitor();

    // Run tray event loop
    tray_->run();

    return 0;
}

void TrayApp::shutdown() {
    should_exit_ = true;
    stop_status_monitor();

    if (tray_) {
        tray_->stop();
    }
}

void TrayApp::build_menu() {
    Menu menu = create_menu();
    tray_->set_menu(menu);
}

Menu TrayApp::create_menu() {
    Menu menu;

    // Status header
    if (server_running_) {
        std::string version = client_->get_version();
        std::string status = "Server Running";
        if (!version.empty()) {
            status += " (v" + version + ")";
        }
        menu.add_item(MenuItem::Action(status, nullptr, false));
        menu.add_item(MenuItem::Action("Port: " + std::to_string(port_), nullptr, false));
    } else {
        menu.add_item(MenuItem::Action("Server Not Running", nullptr, false));
    }

    menu.add_separator();

    // Server control
    if (server_running_) {
        // Loaded models submenu
        auto loaded_models = client_->get_loaded_models();
        if (!loaded_models.empty()) {
            auto loaded_submenu = std::make_shared<Menu>();
            for (const auto& model : loaded_models) {
                std::string label = model.model_name;
                if (!model.device.empty()) {
                    label += " (" + model.device + ")";
                }
                loaded_submenu->add_item(MenuItem::Action(label, nullptr, false));
            }
            loaded_submenu->add_separator();
            loaded_submenu->add_item(MenuItem::Action("Unload All", [this]() {
                if (client_->unload_model()) {
                    show_notification("Models Unloaded", "All models have been unloaded");
                    build_menu();
                }
            }));
            menu.add_item(MenuItem::Submenu("Loaded Models (" + std::to_string(loaded_models.size()) + ")", loaded_submenu));
        }

        // Available models submenu
        auto models = client_->get_models(true);
        if (!models.empty()) {
            auto models_submenu = std::make_shared<Menu>();
            int downloaded_count = 0;

            for (const auto& model : models) {
                if (model.downloaded) {
                    downloaded_count++;
                    models_submenu->add_item(MenuItem::Action(model.id, [this, model]() {
                        on_load_model(model.id);
                    }));
                }
            }

            if (downloaded_count > 0) {
                menu.add_item(MenuItem::Submenu("Load Model", models_submenu));
            }
        }

        menu.add_separator();
        menu.add_item(MenuItem::Action("Stop Server", [this]() { on_stop_server(); }));
    } else {
        menu.add_item(MenuItem::Action("Start Server", [this]() { on_start_server(); }));
    }

    menu.add_separator();

    // Help
    menu.add_item(MenuItem::Action("Documentation", [this]() { on_open_documentation(); }));

    menu.add_separator();

    // Quit
    menu.add_item(MenuItem::Action("Quit", [this]() { on_quit(); }));

    return menu;
}

void TrayApp::on_start_server() {
    if (server_running_) {
        show_notification("Already Running", "Server is already running");
        return;
    }

    if (start_server_process()) {
        // Wait a bit for server to start
        std::this_thread::sleep_for(std::chrono::seconds(2));
        update_status();
        if (server_running_) {
            show_notification("Server Started", "Lemonade Server is now running on port " + std::to_string(port_));
        } else {
            show_notification("Start Failed", "Failed to start server");
        }
    } else {
        show_notification("Start Failed", "Could not start server process");
    }
    build_menu();
}

void TrayApp::on_stop_server() {
    if (!server_running_) {
        show_notification("Not Running", "Server is not running");
        return;
    }

    stop_server_process();
    std::this_thread::sleep_for(std::chrono::seconds(1));
    update_status();
    show_notification("Server Stopped", "Lemonade Server has been stopped");
    build_menu();
}

void TrayApp::on_load_model(const std::string& model_name) {
    show_notification("Loading Model", "Loading " + model_name + "...");

    // Load in background to not block UI
    std::thread([this, model_name]() {
        if (client_->load_model(model_name)) {
            show_notification("Model Loaded", model_name + " is ready");
        } else {
            show_notification("Load Failed", "Failed to load " + model_name);
        }
        // Rebuild menu on main thread
        // Note: This may need platform-specific handling
    }).detach();
}

void TrayApp::on_unload_model(const std::string& model_name) {
    if (client_->unload_model(model_name)) {
        show_notification("Model Unloaded", model_name + " has been unloaded");
    } else {
        show_notification("Unload Failed", "Failed to unload " + model_name);
    }
    build_menu();
}

void TrayApp::on_open_documentation() {
    open_url("https://lemonade-server.ai/");
}

void TrayApp::on_quit() {
    shutdown();
}

void TrayApp::open_url(const std::string& url) {
#ifdef _WIN32
    ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#elif defined(__APPLE__)
    std::string cmd = "open \"" + url + "\"";
    system(cmd.c_str());
#else
    std::string cmd = "xdg-open \"" + url + "\" &";
    system(cmd.c_str());
#endif
}

void TrayApp::show_notification(const std::string& title, const std::string& message) {
    if (tray_) {
        tray_->show_notification(title, message);
    }
}

bool TrayApp::find_server_binary() {
    std::vector<std::string> search_paths;

#ifdef _WIN32
    std::string binary_name = "lemonade-server.exe";
    char exe_path[MAX_PATH];
    if (GetModuleFileNameA(NULL, exe_path, MAX_PATH)) {
        fs::path exe_dir = fs::path(exe_path).parent_path();
        search_paths.push_back((exe_dir / binary_name).string());
    }
#else
    std::string binary_name = "lemonade-server";
    char exe_path[1024];
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len != -1) {
        exe_path[len] = '\0';
        fs::path exe_dir = fs::path(exe_path).parent_path();
        search_paths.push_back((exe_dir / binary_name).string());
    }
#endif

    search_paths.push_back(binary_name);

#ifdef _WIN32
    search_paths.push_back("C:/Program Files/Lemonade/" + binary_name);
#else
    search_paths.push_back("/opt/bin/" + binary_name);
    search_paths.push_back("/usr/bin/" + binary_name);
#endif

    for (const auto& path : search_paths) {
        if (fs::exists(path)) {
            server_binary_ = fs::absolute(path).string();
            if (log_level_ == "debug") {
                std::cout << "[TrayApp] Found server binary: " << server_binary_ << std::endl;
            }
            return true;
        }
    }

    return false;
}

bool TrayApp::start_server_process() {
    if (server_binary_.empty()) {
        if (!find_server_binary()) {
            std::cerr << "[TrayApp] Could not find lemonade-server binary" << std::endl;
            return false;
        }
    }

#ifdef _WIN32
    std::string cmdline = "\"" + server_binary_ + "\" serve --port " + std::to_string(port_);
    if (!host_.empty() && host_ != "127.0.0.1") {
        cmdline += " --host " + host_;
    }

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi = {};

    if (!CreateProcessA(
        nullptr,
        const_cast<char*>(cmdline.c_str()),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &si,
        &pi))
    {
        std::cerr << "[TrayApp] Failed to start server: " << GetLastError() << std::endl;
        return false;
    }

    server_process_ = pi.hProcess;
    CloseHandle(pi.hThread);
    return true;
#else
    pid_t pid = fork();

    if (pid < 0) {
        std::cerr << "[TrayApp] Fork failed" << std::endl;
        return false;
    }

    if (pid == 0) {
        // Child process
        // Redirect stdout/stderr to /dev/null
        int null_fd = open("/dev/null", O_WRONLY);
        if (null_fd >= 0) {
            dup2(null_fd, STDOUT_FILENO);
            dup2(null_fd, STDERR_FILENO);
            close(null_fd);
        }

        std::vector<const char*> args;
        args.push_back(server_binary_.c_str());
        args.push_back("serve");
        args.push_back("--port");
        std::string port_str = std::to_string(port_);
        args.push_back(port_str.c_str());
        if (!host_.empty() && host_ != "127.0.0.1") {
            args.push_back("--host");
            args.push_back(host_.c_str());
        }
        args.push_back("--no-tray");  // Don't show tray since we are the tray
        args.push_back(nullptr);

        execv(server_binary_.c_str(), const_cast<char**>(args.data()));
        exit(1);  // exec failed
    }

    // Parent process
    server_pid_ = pid;
    return true;
#endif
}

void TrayApp::stop_server_process() {
#ifdef _WIN32
    if (server_process_) {
        TerminateProcess(server_process_, 0);
        WaitForSingleObject(server_process_, 5000);
        CloseHandle(server_process_);
        server_process_ = nullptr;
    }
#else
    if (server_pid_ > 0) {
        kill(server_pid_, SIGTERM);
        int status;
        waitpid(server_pid_, &status, 0);
        server_pid_ = 0;
    }
#endif
    server_running_ = false;
}

bool TrayApp::is_server_running() {
    return client_->is_server_running();
}

void TrayApp::update_status() {
    server_running_ = is_server_running();
}

void TrayApp::start_status_monitor() {
    stop_status_monitor_ = false;
    status_thread_ = std::thread([this]() {
        while (!stop_status_monitor_ && !should_exit_) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            if (!stop_status_monitor_ && !should_exit_) {
                bool was_running = server_running_;
                update_status();

                // Notify on status change
                if (was_running && !server_running_) {
                    show_notification("Server Stopped", "Lemonade Server has stopped");
                } else if (!was_running && server_running_) {
                    show_notification("Server Started", "Lemonade Server is now running");
                }
            }
        }
    });
}

void TrayApp::stop_status_monitor() {
    stop_status_monitor_ = true;
    if (status_thread_.joinable()) {
        status_thread_.join();
    }
}

} // namespace tray
