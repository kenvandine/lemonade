#pragma once

#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace tray {

// Information about a loaded model
struct LoadedModelInfo {
    std::string model_name;
    std::string checkpoint;
    std::string type;  // "llm", "embedding", "reranking", "audio", "image"
    std::string device;
    std::string backend_url;
};

// Information about an available model
struct ModelInfo {
    std::string id;
    std::string checkpoint;
    std::string recipe;
    bool downloaded;
};

// Server client for communicating with lemonade-server via HTTP API
class ServerClient {
public:
    ServerClient(const std::string& host = "127.0.0.1", int port = 8000);
    ~ServerClient() = default;

    // Connection settings
    void set_host(const std::string& host) { host_ = host; }
    void set_port(int port) { port_ = port; }
    std::string get_host() const { return host_; }
    int get_port() const { return port_; }

    // Server status
    bool is_server_running();
    nlohmann::json get_health();
    std::string get_version();

    // Model management
    std::vector<ModelInfo> get_models(bool show_all = false);
    std::vector<LoadedModelInfo> get_loaded_models();
    bool load_model(const std::string& model_name, const nlohmann::json& options = nlohmann::json::object());
    bool unload_model(const std::string& model_name = "");

    // Log level
    bool set_log_level(const std::string& level);

private:
    std::string make_request(
        const std::string& endpoint,
        const std::string& method = "GET",
        const std::string& body = "",
        int timeout_seconds = 5
    );

    std::string host_;
    int port_;
    std::string api_key_;
};

} // namespace tray
