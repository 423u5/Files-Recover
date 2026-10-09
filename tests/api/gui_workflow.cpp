#include "gui_workflow.hpp"

#include <chrono>
#include <utility>

namespace gui {

namespace {

using namespace recovery::api;
using namespace std::chrono_literals;

constexpr std::chrono::milliseconds kWait = 120s;

// Records a step; on failure, records why and returns false.
class Steps {
public:
    explicit Steps(WorkflowResult& result) : result_(result) {}

    bool check(bool ok, const std::string& step, const std::string& why = {}) {
        result_.steps.push_back(step);
        if (!ok && result_.error.empty()) {
            result_.error = step + (why.empty() ? std::string(": failed") : ": " + why);
        }
        return ok;
    }

    template <class T>
    bool check(const recovery::Result<T>& result, const std::string& step) {
        return check(result.ok(), step, result.ok() ? std::string() : recovery::describe(result.error()));
    }

private:
    WorkflowResult& result_;
};

// Waits for the session's operation to end; true when it completed.
bool waitCompleted(RecoveryApi& api, const std::string& session, Steps& steps, const std::string& what) {
    const recovery::Result<Progress> progress = api.waitForOperation(session, kWait);
    if (!steps.check(progress, "wait for the " + what)) {
        return false;
    }
    return steps.check(progress->state == OperationState::Completed, what + " completed",
                       std::string("it ended ") + std::string(toString(progress->state)) +
                           (progress->error.has_value() ? ": " + recovery::describe(*progress->error) : ""));
}

}  // namespace

WorkflowResult runWorkflow(RecoveryApi& api, const SourceRef& source, const std::filesystem::path& destination,
                           const std::filesystem::path& report) {
    WorkflowResult result;
    Steps steps(result);

    // The sources to choose from.
    const recovery::Result<std::vector<DiskInfo>> disks = api.listSources();
    if (!steps.check(disks, "list the disks")) {
        return result;
    }
    result.disks = disks->size();

    // What the chosen source holds.
    const recovery::Result<SourceInspection> inspection = api.inspectSource(source);
    if (!steps.check(inspection, "inspect the source")) {
        return result;
    }
    result.volumes = inspection->volumes.size();

    // A deep scan, paused and resumed once.
    ScanSettings settings;
    settings.mode = ScanMode::Deep;
    const recovery::Result<std::string> session = api.startScan(source, settings);
    if (!steps.check(session, "start the scan")) {
        return result;
    }
    result.session = *session;
    if (api.pauseScan(result.session).ok()) {
        const recovery::Result<Progress> paused = api.getProgress(result.session);
        result.paused = paused.ok() && paused->state == OperationState::Paused;
        if (!steps.check(api.resumeScan(result.session), "resume the scan")) {
            return result;
        }
    }
    if (!waitCompleted(api, result.session, steps, "scan")) {
        return result;
    }

    // What was found: the list, each file's details and a preview.
    CandidateQuery query;
    query.count = 100;
    const recovery::Result<CandidatePage> page = api.getCandidates(result.session, query);
    if (!steps.check(page, "list the candidates")) {
        return result;
    }
    result.candidates = page->candidates.size();
    for (const CandidateInfo& candidate : page->candidates) {
        const recovery::Result<CandidateDetails> details = api.getCandidateDetails(result.session, candidate.id);
        if (!steps.check(details, "details of candidate " + std::to_string(candidate.id.value))) {
            return result;
        }
        if (!details->media.has_value()) {
            continue;
        }
        const std::vector<PreviewInfo>& previews = details->media->previews;
        for (std::size_t i = 0; i < previews.size(); ++i) {
            if (previews[i].kind != PreviewKind::Content) {
                continue;
            }
            const recovery::Result<std::vector<std::byte>> bytes =
                api.readPreview(result.session, candidate.id, i, 0, 4096);
            if (!steps.check(bytes, "preview of candidate " + std::to_string(candidate.id.value))) {
                return result;
            }
            if (!bytes->empty()) {
                ++result.previews;
            }
        }
    }

    // Everything, to the destination folder.
    RecoveryOptions recoveryOptions;
    recoveryOptions.destination = destination;
    const recovery::Result<RecoveryStart> start = api.recoverAll(result.session, recoveryOptions);
    if (!steps.check(start, "recover every candidate")) {
        return result;
    }
    if (!steps.check(!start->jobs.empty(), "a recovery job runs")) {
        return result;
    }
    if (!waitCompleted(api, result.session, steps, "recovery")) {
        return result;
    }
    const recovery::Result<CandidatePage> after = api.getCandidates(result.session, query);
    if (!steps.check(after, "list the candidates after recovery")) {
        return result;
    }
    for (const CandidateInfo& candidate : after->candidates) {
        if (candidate.recovery == RecoveryState::Recovered) {
            ++result.recovered;
            result.files.push_back(candidate.recoveredFile);
        }
    }

    // The report, and the session's state.
    ReportOptions reportOptions;
    reportOptions.format = ReportFormat::Json;
    if (!steps.check(api.exportReport(result.session, report, reportOptions), "export the report")) {
        return result;
    }
    const recovery::Result<SessionDetails> details = api.getSession(result.session);
    if (!steps.check(details, "the session's details")) {
        return result;
    }
    if (!steps.check(details->scan.state == RunState::Completed && details->jobs.size() == 1,
                     "the session records a completed scan and one recovery job")) {
        return result;
    }
    steps.check(api.closeSession(result.session), "close the session");
    return result;
}

}  // namespace gui
