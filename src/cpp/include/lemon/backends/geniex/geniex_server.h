#pragma once

#include "lemon/backends/backend_registry.h"

#include "lemon/wrapped_server.h"
#include "lemon/backends/backend_utils.h"
#include <string>

namespace lemon {
namespace backends {

// Wraps Qualcomm GenieX's `geniex serve`, an OpenAI-compatible local HTTP
// server backed by GenieX's `llama_cpp` model path (GGUF, NPU/GPU/CPU on
// Snapdragon). GenieX manages its own on-disk model cache (populated via
// `geniex pull`) rather than accepting an arbitrary file path at launch, so
// unlike LlamaCppServer, load() drives that pull itself instead of relying on
// Lemonade's Hugging Face download path (see BackendDescriptor::self_manages_downloads).
class GenieXServer : public WrappedServer {
public:
    static InstallParams get_install_params(const std::string& backend, const std::string& version);

    GenieXServer(const std::string& log_level,
                 ModelManager* model_manager,
                 BackendManager* backend_manager);

    ~GenieXServer() override;

    // Runs `geniex pull <checkpoint> [--force]` to register the model in
    // GenieX's cache before it can be referenced by `geniex serve`. exe_path
    // must be the already-resolved binary for the compute unit being used;
    // pulling doesn't itself depend on the compute unit, but reusing the
    // resolved path avoids re-resolving against a backend that may not be
    // the one actually installed/supported on this system.
    void pull_model(const std::string& exe_path, const std::string& checkpoint, bool do_not_upgrade);

    void load(const std::string& model_name,
             const ModelInfo& model_info,
             const RecipeOptions& options,
             bool do_not_upgrade = false) override;

    void unload() override;

    // ICompletionServer implementation
    json chat_completion(const json& request) override;
    json completion(const json& request) override;
    json responses(const json& request) override;

    // Streaming requests (used by the web UI and any client sending
    // `stream: true`) bypass chat_completion()/completion() and go straight
    // through Router::chat_completion_stream() -> forward_streaming_request(),
    // so the model-id rewrite has to be applied here too (see FastFlowLMServer
    // for the same pattern).
    void forward_streaming_request(const std::string& endpoint,
                                   const std::string& request_body,
                                   httplib::DataSink& sink,
                                   bool sse = true,
                                   long timeout_seconds = 0,
                                   TelemetryCallback telemetry_callback = nullptr) override;

private:
    std::string resolve_binary_path(const std::string& backend);

    // geniex's /v1/chat/completions and /v1/completions look models up by the
    // checkpoint id it was pulled under (e.g. "unsloth/Qwen3-0.6B-GGUF:Q4_0"),
    // not Lemonade's model name (e.g. "Qwen3-0.6B-GenieX-GGUF"), so the
    // "model" field must be rewritten before forwarding.
    json with_geniex_model_id(const json& request) const;

    bool is_loaded_ = false;
};

namespace geniex {
// Factory for the geniex backend (constructs the server class — lemond only).
std::unique_ptr<WrappedServer> create(const BackendContext& ctx);
const BackendSpec* spec();
const BackendOps* ops();
constexpr uint32_t capabilities() { return capability_mask_of<GenieXServer>(); }
}  // namespace geniex
}  // namespace backends
}  // namespace lemon
