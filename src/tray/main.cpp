#include "tray/tray_app.h"
#include <CLI/CLI.hpp>
#include <iostream>
#include <cstdlib>

int main(int argc, char* argv[]) {
    CLI::App app{"lemonade-server-tray - System tray for Lemonade Server"};

    int port = 8000;
    std::string host = "127.0.0.1";
    std::string log_level = "info";

    app.add_option("-p,--port", port, "Server port to connect to")
        ->envname("LEMONADE_PORT")
        ->default_val(port);

    app.add_option("-H,--host", host, "Server host to connect to")
        ->envname("LEMONADE_HOST")
        ->default_val(host);

    app.add_option("-l,--log-level", log_level, "Log level (debug, info, warning, error)")
        ->check(CLI::IsMember({"debug", "info", "warning", "error"}))
        ->default_val(log_level);

    CLI11_PARSE(app, argc, argv);

    try {
        tray::TrayApp tray_app(host, port);
        tray_app.set_log_level(log_level);
        return tray_app.run();
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "Unknown fatal error" << std::endl;
        return 1;
    }
}
