#pragma once

// P19's acceptance: a user interface's whole workflow, written against the
// GUI-facing API alone. This file and gui_workflow.cpp are compiled with an
// include path that holds nothing but the API's public headers (and the
// engine's error types they use): no filesystem, carving, session or storage
// header can be reached, so the workflow cannot depend on them.

#include "api/recovery_api.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace gui {

struct WorkflowResult {
    // What the user interface did, step by step (for the test's messages).
    std::vector<std::string> steps;
    // Empty: every step succeeded.
    std::string error;
    std::string session;
    std::size_t disks = 0;
    std::size_t volumes = 0;
    bool paused = false;
    std::size_t candidates = 0;
    std::size_t previews = 0;
    std::size_t recovered = 0;
    std::vector<std::filesystem::path> files;
};

// Lists the disks, inspects `source`, scans it (Deep; pausing and resuming
// it once), lists what the scan found with each file's details and the
// first bytes of its content preview, recovers everything to `destination`,
// exports the report to `report`, and closes the session.
[[nodiscard]] WorkflowResult runWorkflow(recovery::api::RecoveryApi& api, const recovery::api::SourceRef& source,
                                         const std::filesystem::path& destination,
                                         const std::filesystem::path& report);

}  // namespace gui
