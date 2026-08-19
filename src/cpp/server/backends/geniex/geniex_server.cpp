#include "lemon/backends/geniex/geniex_server.h"
#include "lemon/backends/geniex/geniex.h"
#include "lemon/backends/backend_registry.h"
#include "lemon/backends/backend_ops.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/backend_manager.h"
#include "lemon/error_types.h"
#include "lemon/model_manager.h"
#include "lemon/runtime_config.h"
#include "lemon/system_info.h"
#include "lemon/utils/custom_args.h"
#include "lemon/utils/process_manager.h"
#include <lemon/utils/aixlog.hpp>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <filesystem>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace lemon {
namespace backends {

namespace {

// Builds the `LD_LIBRARY_PATH=... ` command prefix (Linux only) needed
// because geniex ships its dependent libgeniex.so next to the binary rather
// than via RPATH.
std::string ld_library_path_prefix(const std::string& exe_path) {
#ifndef _WIN32
    std::string lib_path = std::filesystem::path(exe_path).parent_path().string();
    const char* existing_ld_path = std::getenv("LD_LIBRARY_PATH");
    if (existing_ld_path && existing_ld_path[0] != '\0') {
        lib_path += std::string(":") + existing_ld_path;
    }
    return "LD_LIBRARY_PATH=\"" + lib_path + "\" ";
#else
    (void)exe_path;
    return "";
#endif
}

// GenieX auto-detects the chipset itself on Windows-on-Snapdragon, Dragonwing
// Linux, and Android, but NOT on Snapdragon X/X2 Elite "Compute" laptops
// running a generic Linux distro (this backend's only supported combination
// today) -- there `geniex config get chipset` reports "unknown" and every
// pull/serve fails with "No chipset configured." The SoC id GenieX expects
// (e.g. "X1E80100") is exposed via the device tree's `compatible` property as
// `qcom,x1e80100`, so read it from there rather than requiring the user to
// run the interactive `geniex config set chipset` picker by hand.
std::string detect_chipset() {
#ifdef __linux__
    std::ifstream f("/sys/firmware/devicetree/base/compatible", std::ios::binary);
    if (!f) return "";
    std::stringstream ss;
    ss << f.rdbuf();
    std::string data = ss.str();

    // NUL-separated list of compatible strings, most-specific first.
    size_t pos = 0;
    while (pos < data.size()) {
        size_t end = data.find('\0', pos);
        if (end == std::string::npos) end = data.size();
        std::string token = data.substr(pos, end - pos);
        pos = end + 1;

        const std::string prefix = "qcom,";
        if (token.rfind(prefix, 0) == 0) {
            std::string soc = token.substr(prefix.size());
            std::transform(soc.begin(), soc.end(), soc.begin(),
                           [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            if (!soc.empty()) return soc;
        }
    }
#endif
    return "";
}

// Configures GenieX's chipset if it isn't already set, using `exe_path`
// (already resolved/installed). No-op if detection fails or a chipset is
// already configured (never overrides an explicit user/auto-detected choice).
void ensure_chipset_configured(const std::string& exe_path) {
    std::string ld_prefix = ld_library_path_prefix(exe_path);

    std::string get_command = ld_prefix + "\"" + exe_path + "\" config get chipset";
    std::string get_output;
    utils::ProcessManager::run_command(get_command, get_output, /*timeout_seconds=*/30);
    // Trim trailing whitespace/newlines.
    while (!get_output.empty() && std::isspace(static_cast<unsigned char>(get_output.back()))) {
        get_output.pop_back();
    }
    if (!get_output.empty() && get_output != "unknown") {
        return;  // already configured; don't override.
    }

    std::string chipset = detect_chipset();
    if (chipset.empty()) {
        return;  // can't detect; leave it for geniex's own error message.
    }

    LOG(INFO, "geniex-server") << "No GenieX chipset configured; auto-detected '"
                               << chipset << "'" << std::endl;
    std::string set_command = ld_prefix + "\"" + exe_path + "\" config set chipset " + chipset;
    std::string set_output;
    int exit_code = utils::ProcessManager::run_command(set_command, set_output, /*timeout_seconds=*/30);
    if (exit_code != 0) {
        LOG(WARNING, "geniex-server") << "Failed to auto-configure GenieX chipset '"
                                      << chipset << "': " << set_output << std::endl;
    }
}

// Returns the list of checkpoint ids (e.g. "unsloth/Qwen3-0.6B-GGUF:Q4_0")
// currently cached by GenieX, as reported by `geniex list --format json`.
// GenieX manages its own on-disk model cache (see
// BackendDescriptor::self_manages_downloads), so "downloaded" status has to
// be queried through the CLI rather than checked via file paths on disk.
std::vector<std::string> geniex_installed_checkpoints(const std::string& exe_path) {
    std::vector<std::string> checkpoints;
    std::string command = ld_library_path_prefix(exe_path) + "\"" + exe_path + "\" list --format json";
    std::string output;
    int exit_code = utils::ProcessManager::run_command(command, output, /*timeout_seconds=*/30);
    if (exit_code != 0) {
        return checkpoints;
    }
    try {
        json parsed = json::parse(output);
        if (!parsed.is_array()) return checkpoints;
        for (const auto& entry : parsed) {
            if (!entry.contains("name") || !entry["name"].is_string()) continue;
            std::string name = entry["name"].get<std::string>();
            if (entry.contains("precisions") && entry["precisions"].is_array()) {
                for (const auto& precision : entry["precisions"]) {
                    if (precision.is_string()) {
                        checkpoints.push_back(name + ":" + precision.get<std::string>());
                    }
                }
            } else {
                checkpoints.push_back(name);
            }
        }
    } catch (...) {}
    return checkpoints;
}

}  // namespace

InstallParams GenieXServer::get_install_params(const std::string& backend, const std::string& version) {
    (void)backend;  // one CLI binary covers npu/gpu/cpu; --compute selects the unit at runtime
    InstallParams params;
    params.repo = "qualcomm/GenieX";
#ifdef __linux__
    params.filename = "geniex-cli-linux-arm64-" + version + ".tar.gz";
#else
    // v1 only supports Linux ARM64. GenieX's Windows ARM64 release asset is an
    // Inno Setup installer, not a plain archive, and needs a separate extraction
    // strategy (see docs/dev/backends-reference.md and the GenieX backend plan).
    throw std::runtime_error("GenieX is currently supported on Linux ARM64 only");
#endif
    return params;
}

GenieXServer::GenieXServer(const std::string& log_level,
                           ModelManager* model_manager,
                           BackendManager* backend_manager)
    : WrappedServer("geniex-server", log_level, model_manager, backend_manager) {}

GenieXServer::~GenieXServer() {
    unload();
}

std::string GenieXServer::resolve_binary_path(const std::string& backend) {
    const BackendSpec* spec = geniex::spec();
    std::string external = BackendUtils::find_external_backend_binary(spec->recipe, backend);
    if (!external.empty() && std::filesystem::exists(external)) {
        return external;
    }
    backend_manager_->install_backend(spec->recipe, backend);
    return BackendUtils::get_backend_binary_path(*spec, backend);
}

void GenieXServer::pull_model(const std::string& exe_path, const std::string& checkpoint, bool do_not_upgrade) {
    LOG(INFO, "geniex-server") << "Pulling model with GenieX: " << checkpoint << std::endl;
    // `geniex pull` has no re-download/force flag (see `geniex pull --help`), so
    // do_not_upgrade currently has no effect: geniex itself decides whether an
    // already-cached model needs refreshing.
    (void)do_not_upgrade;

    ensure_chipset_configured(exe_path);

    std::string command = ld_library_path_prefix(exe_path);
    command += "\"" + exe_path + "\" pull \"" + checkpoint + "\"";

    std::string output;
    // Model pulls can be large (multi-GB GGUF files); allow generous time.
    int exit_code = utils::ProcessManager::run_command(command, output, /*timeout_seconds=*/1800);
    if (exit_code != 0) {
        LOG(ERROR, "geniex-server") << "geniex pull failed (exit " << exit_code << "): " << output << std::endl;
        throw std::runtime_error("geniex pull failed for '" + checkpoint + "': " + output);
    }
    LOG(INFO, "geniex-server") << "Model pull completed successfully" << std::endl;
}

void GenieXServer::load(const std::string& model_name,
                        const ModelInfo& model_info,
                        const RecipeOptions& options,
                        bool do_not_upgrade) {
    LOG(INFO, "geniex-server") << "Loading model: " << model_name << std::endl;

    std::string backend = options.get_option("geniex_backend");
    if (backend.empty()) {
        auto supported = SystemInfo::get_supported_backends("geniex-llamacpp");
        if (supported.backends.empty()) {
            throw UnsupportedOperationException(
                "GenieX", "this system: no supported NPU/GPU/CPU compute unit detected "
                          "(requires a Qualcomm Snapdragon Linux ARM64 device)");
        }
        backend = supported.backends[0];
    }
    RuntimeConfig::validate_backend_choice("geniex", backend);

    const std::string exe_path = resolve_binary_path(backend);

    // GenieX manages its own model cache; register the checkpoint before serving
    // (see BackendDescriptor::self_manages_downloads).
    pull_model(exe_path, model_info.checkpoint(), do_not_upgrade);

    int ctx_size = options.get_option("ctx_size");
    std::string geniex_args = options.get_option("geniex_args");

    port_ = choose_port();
    if (port_ == 0) {
        throw std::runtime_error("Failed to find an available port");
    }

    std::vector<std::string> args = {
        "serve",
        // geniex's `--host` flag takes a combined "host:port" address; there is
        // no separate `--port` flag (see `geniex serve --help`).
        "--host", "127.0.0.1:" + std::to_string(port_),
        "--compute", backend,
        "--nctx", std::to_string(ctx_size),
    };

    for (const auto& tok : utils::parse_custom_args(geniex_args)) {
        args.push_back(tok);
    }

    std::vector<std::pair<std::string, std::string>> env_vars;
#ifndef _WIN32
    // geniex ships its dependent libgeniex.so next to the binary rather than
    // via RPATH, so it can't be found without this.
    std::string lib_path = std::filesystem::path(exe_path).parent_path().string();
    const char* existing_ld_path = std::getenv("LD_LIBRARY_PATH");
    if (existing_ld_path && existing_ld_path[0] != '\0') {
        lib_path += std::string(":") + existing_ld_path;
    }
    env_vars.push_back({"LD_LIBRARY_PATH", lib_path});
#endif

    LOG(INFO, "geniex-server") << "Starting geniex serve on port " << port_ << std::endl;
    set_process_handle(utils::ProcessManager::start_process(exe_path, args, "", is_debug(), true, env_vars));

    if (!wait_for_ready("/v1/models")) {
        const ProcessHandle handle = consume_process_handle_for_cleanup();
        if (has_process_handle(handle)) {
            utils::ProcessManager::stop_process(handle);
        }
        throw std::runtime_error("geniex serve failed to start or become ready");
    }

    is_loaded_ = true;
    LOG(INFO, "geniex-server") << "Model loaded on port " << get_backend_port() << std::endl;
}

void GenieXServer::unload() {
    stop_backend_watchdog();
    LOG(INFO, "geniex-server") << "Unloading model..." << std::endl;

    const ProcessHandle handle = consume_process_handle_for_cleanup();
    if (has_process_handle(handle)) {
        utils::ProcessManager::stop_process(handle);
    }
    is_loaded_ = false;
}

json GenieXServer::with_geniex_model_id(const json& request) const {
    json out = request;
    out["model"] = get_checkpoint();
    return out;
}

json GenieXServer::chat_completion(const json& request) {
    return forward_request("/v1/chat/completions", with_geniex_model_id(request));
}

json GenieXServer::completion(const json& request) {
    return forward_request("/v1/completions", with_geniex_model_id(request));
}

json GenieXServer::responses(const json& request) {
    // GenieX's local server documents only /v1/chat/completions and
    // /v1/completions; it does not implement the Responses API.
    (void)request;
    return ErrorResponse::from_exception(
        UnsupportedOperationException("Responses API", "geniex")
    );
}

void GenieXServer::forward_streaming_request(const std::string& endpoint,
                                             const std::string& request_body,
                                             httplib::DataSink& sink,
                                             bool sse,
                                             long timeout_seconds,
                                             TelemetryCallback telemetry_callback) {
    try {
        json request = json::parse(request_body);
        std::string modified_body = with_geniex_model_id(request).dump();
        WrappedServer::forward_streaming_request(endpoint, modified_body, sink, sse,
                                                 timeout_seconds, telemetry_callback);
    } catch (const json::exception&) {
        // If JSON parsing fails, forward the original request unmodified.
        WrappedServer::forward_streaming_request(endpoint, request_body, sink, sse,
                                                 timeout_seconds, telemetry_callback);
    }
}

}  // namespace backends

namespace backends {

namespace {
class GenieXOps : public BackendOps {
public:
    bool is_downloaded(const ModelInfo& info, const BackendOpsContext&) const override {
        auto supported = SystemInfo::get_supported_backends("geniex-llamacpp");
        if (supported.backends.empty()) {
            return false;
        }
        const std::string backend = supported.backends[0];
        const BackendSpec* spec = geniex::spec();
        std::string exe_path = BackendUtils::find_external_backend_binary(spec->recipe, backend);
        if (exe_path.empty() || !std::filesystem::exists(exe_path)) {
            exe_path = BackendUtils::get_backend_binary_path(*spec, backend);
        }
        if (exe_path.empty() || !std::filesystem::exists(exe_path)) {
            return false;
        }
        const auto installed = geniex_installed_checkpoints(exe_path);
        return std::find(installed.begin(), installed.end(), info.checkpoint()) != installed.end();
    }

    void download_model(const ModelInfo& info, bool do_not_upgrade, DownloadProgressCallback progress,
                        const BackendOpsContext&) const override {
        (void)progress;
        // GenieX pulls models itself via `geniex pull`; there's no running
        // WrappedServer instance at download time, so resolve/install the
        // binary and shell out directly rather than reusing GenieXServer::pull_model.
        // Resolve against whichever compute unit is actually supported/installed
        // on this system rather than assuming "npu" (Qualcomm NPU/GPU detection
        // doesn't exist yet, so "npu" is unsupported on every system today).
        auto supported = SystemInfo::get_supported_backends("geniex-llamacpp");
        if (supported.backends.empty()) {
            throw UnsupportedOperationException(
                "GenieX", "this system: no supported NPU/GPU/CPU compute unit detected "
                          "(requires a Qualcomm Snapdragon Linux ARM64 device)");
        }
        const std::string backend = supported.backends[0];

        const BackendSpec* spec = geniex::spec();
        std::string exe_path = BackendUtils::find_external_backend_binary(spec->recipe, backend);
        if (exe_path.empty() || !std::filesystem::exists(exe_path)) {
            exe_path = BackendUtils::get_backend_binary_path(*spec, backend);
        }

        ensure_chipset_configured(exe_path);

        std::string command = ld_library_path_prefix(exe_path);
        command += "\"" + exe_path + "\" pull \"" + info.checkpoint() + "\"";
        // `geniex pull` has no re-download/force flag (see `geniex pull --help`).
        (void)do_not_upgrade;
        std::string output;
        int exit_code = utils::ProcessManager::run_command(command, output, /*timeout_seconds=*/1800);
        if (exit_code != 0) {
            throw std::runtime_error("geniex pull failed for '" + info.checkpoint() + "': " + output);
        }
    }
};
}  // namespace

namespace geniex {

std::unique_ptr<WrappedServer> create(const BackendContext& ctx) {
    return make_server<GenieXServer>(ctx);
}

const BackendSpec* spec() { return make_spec<GenieXServer>(descriptor); }
const BackendOps* ops() { return single_ops<GenieXOps>(); }

}  // namespace geniex
}  // namespace backends
}  // namespace lemon
