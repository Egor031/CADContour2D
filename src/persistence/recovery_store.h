#pragma once

#include "project_open.h"

#include <functional>

class QFileDevice;

namespace cadcontour::persistence {

enum class RecoveryDisposition { Open, Closed, Rejected };
enum class RecoverySessionState { Active, Interrupted, Closed, Rejected, Invalid };
enum class RecoveryRelation { Unsaved, MatchesExplicit, Modified, Conflict, Unknown };
struct RecoveryLimits {
    ManifestLimits manifest{4ULL << 20, 4096, 32};
    std::uint64_t maxRecordBytes = 12ULL << 20;
    std::uint32_t maxRecords = 1024;
    std::uint64_t maxDiscoveryBytes = 64ULL << 20;
};

// Kernel-only lifetime marker: destruction performs no filesystem I/O. The marker
// disappears on process termination, including a crash; UUIDs prevent PID reuse.
class RecoveryLease {
public:
    ~RecoveryLease();
    static Result<std::shared_ptr<const RecoveryLease>> acquire(QUuid session);
    static Result<bool> active(QUuid session);
    QUuid sessionId() const noexcept { return session_; }
private:
    RecoveryLease(void *handle, QUuid session) : handle_(handle), session_(session) {}
    void *handle_;
    QUuid session_;
};
struct RecoveryRecord {
    QUuid sessionId;
    RecoveryDisposition disposition = RecoveryDisposition::Open;
    std::shared_ptr<const SavedProject> state;
    std::shared_ptr<const SavedProject> baseline;
    QByteArray digest;
    bool requiresSaveAs = false; // established conflict; baseline is provenance, not a saved state
};
struct RecoveryCandidate {
    QUuid sessionId;
    RecoverySessionState session = RecoverySessionState::Invalid;
    RecoveryRelation relation = RecoveryRelation::Unknown;
    std::shared_ptr<const RecoveryRecord> record;
    std::optional<Error> error;
};
struct RestoredRecovery {
    LoadedProject project; // state/physical binding, not the explicit baseline
    std::shared_ptr<const SavedProject> baseline;
    std::shared_ptr<const RecoveryRecord> source;
    std::shared_ptr<const RecoveryLease> lease;
    bool conflict = false;
};

// All functions below perform bounded metadata I/O and belong on the managed worker.
// Discovery never claims, accepts, rejects, removes or probes datasets.
Result<std::vector<RecoveryCandidate>> discoverRecovery(const QString &root, RecoveryLimits limits = {},
    cache::CacheReader::CancelCheck cancel = nullptr, void *context = nullptr);
Result<RestoredRecovery> restoreRecovery(const QString &root, const RecoveryCandidate &candidate,
    RecoveryLimits limits, cache::ResourceLimits readerLimits,
    cache::CacheReader::CancelCheck cancel = nullptr, void *context = nullptr);
Status rejectRecovery(const QString &root, const RecoveryCandidate &candidate, RecoveryLimits limits = {});
Status closeRecovery(const QString &root, QUuid session, RecoveryLimits limits = {},
    std::shared_ptr<const RecoveryLease> lease = {}, bool discardConfirmed = false);
QString recoveryFileName(QUuid session);

enum class RecoveryPhase { Target, Open, Write, Flush, BeforeCommit, Published, Cleanup };
struct RecoveryReceipt {
    bool published = false;
    bool cleanupWarning = false;
    std::optional<Error> error;
};
class RecoveryTransactionTestAccess;
class RecoveryTransaction {
public:
    static Result<std::unique_ptr<RecoveryTransaction>> prepare(QString root, RecoveryRecord record,
        std::shared_ptr<const RecoveryLease> lease, RecoveryLimits limits,
        std::function<bool()> cancelled = {});
    ~RecoveryTransaction();
    const std::shared_ptr<const RecoveryRecord> &prepared() const noexcept;
    const std::shared_ptr<const RecoveryLease> &lease() const noexcept;
    RecoveryReceipt commit() noexcept; // after owner permission; never changes domain state
private:
    friend class RecoveryTransactionTestAccess;
    struct TestHooks {
        std::function<void(RecoveryPhase)> phase;
        std::function<qint64(QIODevice &, const char *, qint64)> write;
        std::function<bool(QFileDevice &)> flush;
        std::function<bool(QSaveFile &)> commit;
    };
    struct Impl;
    explicit RecoveryTransaction(std::unique_ptr<Impl> impl);
    static Result<std::unique_ptr<RecoveryTransaction>> prepareImpl(QString root, RecoveryRecord record,
        std::shared_ptr<const RecoveryLease> lease, RecoveryLimits limits,
        std::function<bool()> cancelled, std::shared_ptr<TestHooks> hooks);
    std::unique_ptr<Impl> impl_;
};

} // namespace cadcontour::persistence
