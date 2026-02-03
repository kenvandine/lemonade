#include "tray/server_client.h"
#include <httplib.h>
#include <iostream>
#include <cstdlib>

namespace tray {

ServerClient::ServerClient(const std::string& host, int port)
    : host_(host)
    , port_(port)
{
    const char* api_key_env = std::getenv("LEMONADE_API_KEY");
    api_key_ = api_key_env ? std::string(api_key_env) : "";
}

std::string ServerClient::make_request(
    const std::string& endpoint,
    const std::string& method,
    const std::string& body,
    int timeout_seconds)
{
    httplib::Client cli(host_, port_);
    cli.set_connection_timeout(5, 0);  // 5 second connection timeout
    cli.set_read_timeout(timeout_seconds, 0);

    if (!api_key_.empty()) {
        cli.set_bearer_token_auth(api_key_);
    }

    httplib::Result res;

    if (method == "GET") {
        res = cli.Get(endpoint.c_str());
    } else if (method == "POST") {
        res = cli.Post(endpoint.c_str(), body, "application/json");
    } else {
        throw std::runtime_error("Unsupported HTTP method: " + method);
    }

    if (!res) {
        auto err = res.error();
        std::string error_msg;
        switch (err) {
            case httplib::Error::Connection:
                error_msg = "Failed to connect to server at " + host_ + ":" + std::to_string(port_);
                break;
            case httplib::Error::Read:
                error_msg = "Server connection closed";
                break;
            default:
                error_msg = "HTTP request failed (error code: " + std::to_string(static_cast<int>(err)) + ")";
                break;
        }
        throw std::runtime_error(error_msg);
    }

    if (res->status != 200) {
        std::string error_msg = "HTTP request failed with status: " + std::to_string(res->status);
        try {
            auto error_json = nlohmann::json::parse(res->body);
            if (error_json.contains("error")) {
                error_msg = error_json["error"].get<std::string>();
            } else if (error_json.contains("detail")) {
                error_msg = error_json["detail"].get<std::string>();
            }
        } catch (...) {
            if (!res->body.empty() && res->body.length() < 200) {
                error_msg += ": " + res->body;
            }
        }
        throw std::runtime_error(error_msg);
    }

    return res->body;
}

bool ServerClient::is_server_running() {
    try {
        get_health();
        return true;
    } catch (...) {
        return false;
    }
}

nlohmann::json ServerClient::get_health() {
    std::string response = make_request("/api/v1/health");
    return nlohmann::json::parse(response);
}

std::string ServerClient::get_version() {
    try {
        auto health = get_health();
        if (health.contains("version")) {
            return health["version"].get<std::string>();
        }
    } catch (...) {
    }
    return "";
}

std::vector<ModelInfo> ServerClient::get_models(bool show_all) {
    std::vector<ModelInfo> models;

    try {
        std::string endpoint = "/api/v1/models";
        if (show_all) {
            endpoint += "?show_all=true";
        }
        std::string response = make_request(endpoint);
        auto json = nlohmann::json::parse(response);

        if (json.contains("data") && json["data"].is_array()) {
            for (const auto& model : json["data"]) {
                ModelInfo info;
                info.id = model.value("id", "");
                info.checkpoint = model.value("checkpoint", "");
                info.recipe = model.value("recipe", "");
                info.downloaded = model.value("downloaded", false);
                models.push_back(info);
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[ServerClient] Failed to get models: " << e.what() << std::endl;
    }

    return models;
}

std::vector<LoadedModelInfo> ServerClient::get_loaded_models() {
    std::vector<LoadedModelInfo> models;

    try {
        auto health = get_health();

        if (health.contains("loaded_models") && health["loaded_models"].is_array()) {
            for (const auto& model : health["loaded_models"]) {
                LoadedModelInfo info;
                info.model_name = model.value("model_name", "");
                info.checkpoint = model.value("checkpoint", "");
                info.type = model.value("type", "llm");
                info.device = model.value("device", "");
                info.backend_url = model.value("backend_url", "");
                models.push_back(info);
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[ServerClient] Failed to get loaded models: " << e.what() << std::endl;
    }

    return models;
}

bool ServerClient::load_model(const std::string& model_name, const nlohmann::json& options) {
    try {
        nlohmann::json load_req = nlohmann::json::object();
        load_req["model_name"] = model_name;

        for (auto& [key, opt] : options.items()) {
            load_req[key] = opt;
        }

        std::string body = load_req.dump();
        make_request("/api/v1/load", "POST", body, 86400);  // 24 hour timeout for large models
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[ServerClient] Failed to load model: " << e.what() << std::endl;
        return false;
    }
}

bool ServerClient::unload_model(const std::string& model_name) {
    try {
        std::string body;
        if (!model_name.empty()) {
            body = "{\"model_name\": \"" + model_name + "\"}";
        }
        make_request("/api/v1/unload", "POST", body, 30);
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[ServerClient] Failed to unload model: " << e.what() << std::endl;
        return false;
    }
}

bool ServerClient::set_log_level(const std::string& level) {
    try {
        std::string body = "{\"level\": \"" + level + "\"}";
        make_request("/api/v1/log-level", "POST", body);
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace tray
