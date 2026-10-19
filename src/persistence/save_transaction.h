#pragma once

#include "project_storage.h"

#include <array>
#include <functional>
#include <memory>

class QSaveFile;

namespace cadcontour::persistence {

enum class SavePhase { Target, Staging, Source, CopyRead, CopyWrite, CopyVerify,
                       DatasetPublish, DatasetPublished, ManifestPrepare, ManifestWrite,
                       BeforeCommit, ManifestCommit, Receipt, Cleanup };
enum class SaveErrorCode { InvalidTarget, Conflict, Locked, MissingDataset, ChangedDataset,
                           FileIO, ResourceUnavailable, InvalidSnapshot, Cancelled, PublicationUncertain };
struct SaveError {
    SaveErrorCode code = SaveErrorCode::FileIO;
    SavePhase phase = SavePhase::Target;
    std::uint32_t systemCode = 0;
    std::array<char, 192> detail{};
};
template<class T> class SaveResult {
public:
    SaveResult(T value) : data_(std::move(value)) {}
    SaveResult(SaveError error) : data_(error) {}
    explicit operator bool() const noexcept { return std::holds_alternative<T>(data_); }
    T &value() { return std::get<T>(data_); }
    const SaveError &error() const { return std::get<SaveError>(data_); }
private:
    std::variant<T, SaveError> data_;
};

// Observed from a retained read handle. It is not a content digest and cannot
// detect hostile writers restoring metadata or arbitrary hardware corruption.
struct DatasetFileIdentity {
    std::uint64_t volume = 0, file = 0, size = 0, modified = 0, changed = 0;
    bool operator==(const DatasetFileIdentity &) const = default;
};
struct SavedProject {
    ProjectSnapshot snapshot;
    ImportedDataset content; // prepared new binding; location excluded from dirty comparison
    QString manifestPath;
    QByteArray manifestDigest;
    DatasetFileIdentity datasetIdentity;
};
bool matchesSavedContent(const ProjectState &state, const SavedProject &saved) noexcept;

struct SaveInput {
    ProjectSnapshot snapshot;
    ImportedDataset dataset;
    QString targetPath;
    std::shared_ptr<const SavedProject> previous;
    bool saveAs = false;
    ManifestLimits limits{4ULL << 20, 4096, 32}; // overridable application v1 admission profile
};
struct SaveCallbacks {
    std::function<bool()> cancelled;
    std::function<void(std::uint64_t, std::uint64_t)> progress;
};
enum class SavePublication { NotPublished, Persisted, Uncertain };
struct SaveCommitResult {
    SavePublication publication = SavePublication::NotPublished;
    std::optional<SaveError> error;
    bool cleanupWarning = false;
};

class SaveTransactionTestAccess;
// Worker-thread object. Only immutable prepared metadata crosses Ready. Final
// datasets are conservatively retained from rename onward, even without a receipt.
class SaveTransaction {
public:
    static SaveResult<std::unique_ptr<SaveTransaction>> prepare(SaveInput input, SaveCallbacks callbacks);
    ~SaveTransaction();
    const std::shared_ptr<const SavedProject> &prepared() const noexcept;
    SaveCommitResult commit() noexcept; // permission already irrevocable; ignores cancellation
private:
    friend class SaveTransactionTestAccess;
    struct TestHooks {
        std::function<void(SavePhase)> phase;
        std::function<qint64(QIODevice &, const char *, qint64)> write;
        std::function<bool(QSaveFile &)> commit;
    };
    struct Impl;
    explicit SaveTransaction(std::unique_ptr<Impl> impl);
    static SaveResult<std::unique_ptr<SaveTransaction>> prepareImpl(
        SaveInput input, SaveCallbacks callbacks, std::shared_ptr<TestHooks> hooks);
    std::unique_ptr<Impl> impl_;
};

} // namespace cadcontour::persistence
