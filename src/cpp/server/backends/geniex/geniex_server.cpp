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
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace lemon {
namespace backends {

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

void GenieXServer::pull_model(const std::string& checkpoint, bool do_not_upgrade) {
    LOG(INFO, "geniex-server") << "Pulling model with GenieX: " << checkpoint << std::endl;

    // The compute unit doesn't affect the pull itself, but resolve_binary_path
    // also lazily installs GenieX on first use.
    const std::string exe_path = resolve_binary_path("npu");

    std::string command = "\"" + exe_path + "\" pull \"" + checkpoint + "\"";
    if (!do_not_upgrade) {
        command += " --force";
    }

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
    RuntimeConfig::validate_backend_choice("geniex-llamacpp", backend);

    const std::string exe_path = resolve_binary_path(backend);

    // GenieX manages its own model cache; register the checkpoint before serving
    // (see BackendDescriptor::self_manages_downloads).
    pull_model(model_info.checkpoint(), do_not_upgrade);

    int ctx_size = options.get_option("ctx_size");
    std::string geniex_args = options.get_option("geniex_args");

    port_ = choose_port();
    if (port_ == 0) {
        throw std::runtime_error("Failed to find an available port");
    }

    std::vector<std::string> args = {
        "serve",
        "--host", "127.0.0.1",
        "--port", std::to_string(port_),
        "--compute", backend,
        "--nctx", std::to_string(ctx_size),
    };

    for (const auto& tok : utils::parse_custom_args(geniex_args)) {
        args.push_back(tok);
    }

    LOG(INFO, "geniex-server") << "Starting geniex serve on port " << port_ << std::endl;
    set_process_handle(utils::ProcessManager::start_process(exe_path, args, "", is_debug(), true));

    if (!wait_for_ready("/health")) {
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

json GenieXServer::chat_completion(const json& request) {
    return forward_request("/v1/chat/completions", request);
}

json GenieXServer::completion(const json& request) {
    return forward_request("/v1/completions", request);
}

json GenieXServer::responses(const json& request) {
    // GenieX's local server documents only /v1/chat/completions and
    // /v1/completions; it does not implement the Responses API.
    (void)request;
    return ErrorResponse::from_exception(
        UnsupportedOperationException("Responses API", "geniex")
    );
}

}  // namespace backends

namespace backends {

namespace {
class GenieXOps : public BackendOps {
public:
    void download_model(const ModelInfo& info, bool do_not_upgrade, DownloadProgressCallback progress,
                        const BackendOpsContext&) const override {
        (void)progress;
        // GenieX pulls models itself via `geniex pull`; there's no running
        // WrappedServer instance at download time, so resolve/install the
        // binary and shell out directly rather than reusing GenieXServer::pull_model.
        const BackendSpec* spec = geniex::spec();
        std::string exe_path = BackendUtils::find_external_backend_binary(spec->recipe, "npu");
        if (exe_path.empty() || !std::filesystem::exists(exe_path)) {
            exe_path = BackendUtils::get_backend_binary_path(*spec, "npu");
        }
        std::string command = "\"" + exe_path + "\" pull \"" + info.checkpoint() + "\"";
        if (!do_not_upgrade) {
            command += " --force";
        }
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
