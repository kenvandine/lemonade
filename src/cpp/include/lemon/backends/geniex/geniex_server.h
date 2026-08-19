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
    // GenieX's cache before it can be referenced by `geniex serve`.
    void pull_model(const std::string& checkpoint, bool do_not_upgrade);

    void load(const std::string& model_name,
             const ModelInfo& model_info,
             const RecipeOptions& options,
             bool do_not_upgrade = false) override;

    void unload() override;

    // ICompletionServer implementation
    json chat_completion(const json& request) override;
    json completion(const json& request) override;
    json responses(const json& request) override;

private:
    std::string resolve_binary_path(const std::string& backend);

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
