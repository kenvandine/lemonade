#pragma once

#include "lemon/backends/backend_descriptor.h"

namespace lemon {
namespace backends {
namespace geniex {

// Covers only GenieX's `llama_cpp` GGUF model path; the NPU-only `qairt` path
// is not modeled here. The "npu"/"gpu" families below have no detection code
// yet, so those rows are intentionally unmatchable (fail-closed) until real
// Qualcomm hardware detection lands.
inline const BackendDescriptor descriptor = {
    /*recipe*/          "geniex-llamacpp",
    /*display_name*/    "GenieX (llama.cpp)",
    /*binary*/          "geniex",
    /*config_section*/  "geniex",  // differs from recipe "geniex-llamacpp"
    /*default_device*/  DEVICE_NPU,
    /*slot_policy*/     SlotPolicy::ExclusiveNpu,
    /*selectable_backend*/ true,
    /*uses_ctx_size*/   true,
    /*dynamic_models*/  false,
    /*options*/ {
        {"geniex_backend", "--geniex", "", "BACKEND",
         "GenieX compute unit to use (npu, gpu, cpu)", "GenieX Options"},
        {"geniex_args", "--geniex-args", "", "ARGS",
         "Custom arguments to pass to geniex serve", "GenieX Options"},
    },
    /*support*/ {
        {"npu", {"linux"}, {{"qualcomm_npu", {"Hexagon"}}}, "Qualcomm Hexagon NPU (Snapdragon)"},
        {"gpu", {"linux"}, {{"qualcomm_gpu", {"Adreno"}}}, "Qualcomm Adreno GPU (Snapdragon)"},
        {"cpu", {"linux"}, {{"cpu", {"arm64"}}}, "ARM64 CPU (Snapdragon)"},
    },
    /*supported_modes*/ {"chat"},
    /*required_checkpoints*/ {"main"},
    /*default_capabilities*/ {},
    /*experimental*/    true,
    /*web_display_name*/ "GenieX (Qualcomm Snapdragon)",
    /*rocm_channels*/   {},
    /*exposes_prometheus_metrics*/ false,
    /*rocm_requires_cwsr_fix*/ false,
    /*version_policy*/  VersionPolicy::Exact,
    /*self_manages_downloads*/ true,  // GenieX pulls its own models via `geniex pull`
    /*takes_args*/      false,
    /*arg_variants*/    {},
    /*bin_variants*/    {},
    /*config_extra*/    nlohmann::json::object(),
};

}  // namespace geniex
}  // namespace backends
}  // namespace lemon
