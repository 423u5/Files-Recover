#include "api_test_support.hpp"

#include "recovery/text.hpp"
#include "scan/scan_test_support.hpp"
#include "session/recovery_session.hpp"
#include "storage/physical_disk_source.hpp"
#include "support/memory_source.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cwctype>
#include <stdexcept>

namespace recovery::api {

void PrintTo(OperationState state, std::ostream* stream) {
    *stream << toString(state);
}

void PrintTo(OperationKind kind, std::ostream* stream) {
    *stream << toString(kind);
}

void PrintTo(RunState state, std::ostream* stream) {
    *stream << toString(state);
}

void PrintTo(EventKind kind, std::ostream* stream) {
    *stream << toString(kind);
}

void PrintTo(RecoveryState state, std::ostream* stream) {
    *stream << toString(state);
}

}  // namespace recovery::api

namespace recovery::api::test {

namespace {

const Bytes& card() {
    static const Bytes bytes = scan::test::makeCard();
    return bytes;
}

}  // namespace

// ---------------------------------------------------------------------------
// DiskRack
// ---------------------------------------------------------------------------

std::shared_ptr<::recovery::test::FakeDeviceConfig> DiskRack::add(
    std::uint32_t number, Bytes bytes, std::vector<std::pair<std::uint64_t, std::uint64_t>> failing) {
    auto config = std::make_shared<::recovery::test::FakeDeviceConfig>();
    config->geometry = ::recovery::test::diskGeometry(bytes.size());
    config->data = std::move(bytes);
    config->description.vendor = "Simulated";
    config->description.product = "Card Reader";
    config->description.removable = true;
    config->failingRanges = std::move(failing);
    const std::lock_guard lock(mutex_);
    disks_[number] = config;
    return config;
}

void DiskRack::failOpen(std::uint32_t number, Error error) {
    const std::lock_guard lock(mutex_);
    failures_[number] = std::move(error);
}

Result<std::unique_ptr<storage::IDeviceIo>> DiskRack::openReadOnly(const std::filesystem::path& path,
                                                                   storage::DeviceKind kind) {
    if (kind != storage::DeviceKind::PhysicalDisk) {
        return makeError(ErrorCode::InvalidInput, "the rack holds physical disks only");
    }
    const std::optional<std::uint32_t> number = storage::PhysicalDiskSource::parseDevicePath(path.native());
    const std::lock_guard lock(mutex_);
    if (number.has_value()) {
        if (const auto failure = failures_.find(*number); failure != failures_.end()) {
            return failure->second;
        }
        if (const auto disk = disks_.find(*number); disk != disks_.end()) {
            return std::unique_ptr<storage::IDeviceIo>(std::make_unique<::recovery::test::MemoryDeviceIo>(
                disk->second, std::make_shared<::recovery::test::FakeDeviceStats>()));
        }
    }
    // ERROR_FILE_NOT_FOUND, as Windows answers for a disk that is not there.
    return makeError(ErrorCode::IoError, "no such disk", 2);
}

// ---------------------------------------------------------------------------
// EventLog
// ---------------------------------------------------------------------------

EventCallback EventLog::callback() {
    return [this](const Event& event) {
        std::function<void(const Event&)> hook;
        {
            const std::lock_guard lock(mutex_);
            hook = hook_;
        }
        if (hook) {
            hook(event);
        }
        {
            const std::lock_guard lock(mutex_);
            events_.push_back(event);
        }
        changed_.notify_all();
    };
}

void EventLog::setHook(std::function<void(const Event&)> hook) {
    const std::lock_guard lock(mutex_);
    hook_ = std::move(hook);
}

std::vector<Event> EventLog::events() const {
    const std::lock_guard lock(mutex_);
    return events_;
}

std::vector<Event> EventLog::eventsOf(std::string_view session) const {
    std::vector<Event> out;
    for (const Event& event : events()) {
        if (!event.imaging.has_value() && event.session == session) {
            out.push_back(event);
        }
    }
    return out;
}

std::vector<Event> EventLog::eventsOf(ImagingId imaging) const {
    std::vector<Event> out;
    for (const Event& event : events()) {
        if (event.imaging == imaging) {
            out.push_back(event);
        }
    }
    return out;
}

bool EventLog::waitFor(const std::function<bool(const std::vector<Event>&)>& done,
                       std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, timeout, [&] { return done(events_); });
}

std::optional<Event> EventLog::waitForFinished(std::string_view session, std::size_t count) {
    std::optional<Event> found;
    const bool done = waitFor([&](const std::vector<Event>& events) {
        std::size_t seen = 0;
        for (const Event& event : events) {
            if (event.kind == EventKind::OperationFinished && !event.imaging.has_value() &&
                event.session == session && ++seen == count) {
                found = event;
                return true;
            }
        }
        return false;
    });
    EXPECT_TRUE(done) << "no OperationFinished event " << count << " of session " << session;
    return found;
}

void EventLog::clear() {
    const std::lock_guard lock(mutex_);
    events_.clear();
}

// ---------------------------------------------------------------------------
// ApiWorld
// ---------------------------------------------------------------------------

ApiWorld::ApiWorld() {
    options_.progressInterval = std::chrono::milliseconds(0);
    hooks_.diskOpener = rack_;
    hooks_.systemFolder = folder() / "Windows";
    hooks_.diskResolver = [places = places_, mutex = placesMutex_](const std::filesystem::path& path)
        -> Result<std::vector<std::uint32_t>> {
        const std::wstring text = path.lexically_normal().native();
        const std::lock_guard lock(*mutex);
        for (const auto& [folder, disk] : *places) {
            const std::wstring prefix = folder.lexically_normal().native();
            if (text.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), text.begin(),
                                                           [](wchar_t a, wchar_t b) {
                                                               return std::towlower(a) == std::towlower(b);
                                                           })) {
                return std::vector<std::uint32_t>{disk};
            }
        }
        return std::vector<std::uint32_t>{0};
    };
}

ApiWorld::~ApiWorld() {
    api_.reset();
}

std::filesystem::path ApiWorld::cardImage() {
    const std::filesystem::path path = folder() / "card.img";
    if (!cardWritten_) {
        ::recovery::test::writeFile(path, card());
        cardWritten_ = true;
    }
    return path;
}

std::uint32_t ApiWorld::addDisk(Bytes bytes, std::vector<std::pair<std::uint64_t, std::uint64_t>> failing) {
    const std::uint32_t number = nextDisk_++;
    (void)rack_->add(number, std::move(bytes), std::move(failing));
    return number;
}

void ApiWorld::placeOn(const std::filesystem::path& folder, std::uint32_t disk) {
    const std::lock_guard lock(*placesMutex_);
    places_->emplace_back(folder, disk);
}

RecoveryApi& ApiWorld::api() {
    if (!api_) {
        ApiOptions options = options_;
        if (options.sessionsRoot.empty()) {
            options.sessionsRoot = sessions();
        }
        if (!options.onEvent) {
            options.onEvent = log_.callback();
        }
        Result<std::unique_ptr<RecoveryApi>> made = createRecoveryApi(std::move(options), hooks_);
        if (!made.ok()) {
            ADD_FAILURE() << "cannot create the API: " << describe(made.error());
            throw std::runtime_error("cannot create the API");
        }
        api_ = std::move(made).value();
    }
    return *api_;
}

void ApiWorld::restart() {
    api_.reset();
}

std::string ApiWorld::startCardScan(ScanSettings settings) {
    Result<std::string> id = api().startScan(cardSource(), settings);
    EXPECT_TRUE(id.ok()) << (id.ok() ? std::string() : describe(id.error()));
    return id.ok() ? *id : std::string();
}

Progress ApiWorld::waitIdle(std::string_view session) {
    Result<Progress> progress = api().waitForOperation(session, kWait);
    EXPECT_TRUE(progress.ok()) << (progress.ok() ? std::string() : describe(progress.error()));
    if (!progress.ok()) {
        return {};
    }
    EXPECT_NE(progress->state, OperationState::Running) << "still running after the wait";
    return *progress;
}

// ---------------------------------------------------------------------------
// Expectations
// ---------------------------------------------------------------------------

const std::vector<std::string>& expectedCard(ScanMode mode) {
    static const std::vector<std::string> deep = [] {
        ::recovery::test::MemoryStorageSource source(card());
        const bool opened = source.open().ok();
        return opened ? scan::test::describe(scan::test::referenceScan(source, recovery::ScanMode::Deep))
                      : std::vector<std::string>{};
    }();
    static const std::vector<std::string> quick = [] {
        ::recovery::test::MemoryStorageSource source(card());
        const bool opened = source.open().ok();
        return opened ? scan::test::describe(scan::test::referenceScan(source, recovery::ScanMode::Quick))
                      : std::vector<std::string>{};
    }();
    return mode == ScanMode::Deep ? deep : quick;
}

std::vector<std::string> journalCandidates(const std::filesystem::path& root, const std::string& id) {
    Result<std::unique_ptr<session::RecoverySession>> opened = session::RecoverySession::open(root / id);
    EXPECT_TRUE(opened.ok()) << (opened.ok() ? std::string() : describe(opened.error()));
    if (!opened.ok()) {
        return {};
    }
    return scan::test::describe((*opened)->candidates());
}

std::vector<CandidateInfo> allCandidates(RecoveryApi& api, std::string_view session) {
    std::vector<CandidateInfo> all;
    CandidateQuery query;
    query.count = 3;
    for (;;) {
        Result<CandidatePage> page = api.getCandidates(session, query);
        EXPECT_TRUE(page.ok()) << (page.ok() ? std::string() : describe(page.error()));
        if (!page.ok() || page->candidates.empty()) {
            return all;
        }
        all.insert(all.end(), page->candidates.begin(), page->candidates.end());
        query.first += page->candidates.size();
    }
}

CandidateInfo candidateNamed(RecoveryApi& api, std::string_view session, std::string_view name) {
    for (const CandidateInfo& candidate : allCandidates(api, session)) {
        if (candidate.name == name) {
            return candidate;
        }
    }
    ADD_FAILURE() << "no candidate is named " << name;
    return {};
}

}  // namespace recovery::api::test
