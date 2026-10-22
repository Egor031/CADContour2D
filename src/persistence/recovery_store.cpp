#include "recovery_store.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QLockFile>
#include <QSaveFile>

#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <limits>

namespace cadcontour::persistence {
namespace {
Error error(ErrorCode code, const QString &text) { return {code, QStringLiteral("recovery"), text.left(256)}; }
[[noreturn]] void fail(const QString &text) { throw error(ErrorCode::FileIO, text); }
QString markerName(QUuid session)
{ return QStringLiteral("Global\\CADContour2D-Recovery-") + session.toString(QUuid::WithoutBraces); }
QByteArray hash(const QByteArray &bytes) { return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256); }
QString directory(const SavedProject &project)
{ return QString::fromStdWString(project.content.location->storageDirectory.wstring()); }
bool absolutePath(const QString &path)
{
    if (path.isEmpty() || path.size() > 32767 || path.contains(u'\0') || path.contains(u'\\')
        || !QDir::isAbsolutePath(path) || QDir::cleanPath(path) != path) return false;
    // Validate all components after the drive/UNC root using the existing portable path rules.
    auto tail = path;
    if (tail.size() >= 3 && tail[1] == u':' && tail[2] == u'/') tail = tail.mid(3);
    else if (tail.startsWith(u"//")) tail = tail.mid(2);
    else return false;
    return tail.isEmpty() || bool(validateDatasetPath(tail));
}
QByteArray encodedManifest(const SavedProject &project, RecoveryLimits limits)
{
    auto encoded = encodeManifest(project.snapshot, limits.manifest);
    if (!encoded) throw encoded.error();
    if (!project.content.location || !absolutePath(directory(project)))
        throw error(ErrorCode::InvalidReference, QStringLiteral("Unsafe recovery dataset root"));
    return encoded.value();
}
QByteArray encode(const RecoveryRecord &r, RecoveryLimits limits)
{
    if (r.sessionId.isNull() || !r.state || !r.state->content.location || (r.requiresSaveAs && !r.baseline) || (r.baseline &&
        (r.baseline->snapshot.projectId != r.state->snapshot.projectId
         || !absolutePath(r.baseline->manifestPath) || r.baseline->manifestDigest.size() != 32)))
        throw error(ErrorCode::InvalidValue, QStringLiteral("Invalid recovery state/baseline"));
    QByteArray out = "PC2D-RECOVERY 2\n" + r.sessionId.toString(QUuid::WithoutBraces).toLatin1() + '\n';
    out += r.disposition == RecoveryDisposition::Open ? "open\n"
        : r.disposition == RecoveryDisposition::Closed ? "closed\n" : "rejected\n";
    out += directory(*r.state).toUtf8().toBase64() + '\n';
    const auto relative = QString::fromStdWString(r.state->content.location->relativeCachePath.generic_wstring());
    if (auto valid = validateDatasetPath(relative); !valid) throw valid.error();
    out += relative.toUtf8().toBase64() + '\n';
    out += encodedManifest(*r.state, limits).toBase64() + '\n';
    out += (r.baseline ? r.baseline->manifestPath.toUtf8().toBase64() : QByteArray{}) + '\n';
    out += (r.baseline ? encodedManifest(*r.baseline, limits).toBase64() : QByteArray{}) + '\n';
    out += (r.baseline ? r.baseline->manifestDigest.toHex() : QByteArray{}) + '\n';
    out += r.requiresSaveAs ? "1\n" : "0\n";
    out += hash(out).toHex() + '\n';
    if (!limits.maxRecordBytes || std::uint64_t(out.size()) > limits.maxRecordBytes)
        throw error(ErrorCode::ResourceExhausted, QStringLiteral("Recovery record exceeds admission budget"));
    return out;
}
QByteArray unbase64(const QByteArray &bytes)
{
    const auto decoded = QByteArray::fromBase64Encoding(bytes, QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded || decoded.decoded.toBase64() != bytes)
        throw error(ErrorCode::InvalidValue, QStringLiteral("Invalid canonical base64"));
    return decoded.decoded;
}
QString pathText(const QByteArray &bytes)
{
    const auto utf8 = unbase64(bytes);
    const auto path = QString::fromUtf8(utf8);
    if (path.toUtf8() != utf8 || !absolutePath(path))
        throw error(ErrorCode::InvalidReference, QStringLiteral("Invalid UTF-8 absolute recovery path"));
    return path;
}
std::shared_ptr<const RecoveryRecord> decode(const QByteArray &bytes, QUuid expected, RecoveryLimits limits)
{
    const auto parts = bytes.split('\n');
    const bool legacy = parts[0] == "PC2D-RECOVERY 1";
    if (!legacy && parts[0] != "PC2D-RECOVERY 2")
        throw error(ErrorCode::UnsupportedVersion, QStringLiteral("Unsupported recovery format/version"));
    const auto checksumLine = legacy ? 9 : 10;
    if (parts.size() != checksumLine + 2 || !parts.back().isEmpty())
        throw error(ErrorCode::InvalidValue, QStringLiteral("Incomplete recovery envelope"));
    if (parts[1] != expected.toString(QUuid::WithoutBraces).toLatin1()
        || expected.isNull() || parts[checksumLine].size() != 64
        || hash(bytes.left(bytes.size() - 65)).toHex() != parts[checksumLine])
        throw error(ErrorCode::InvalidValue, QStringLiteral("Recovery identity/checksum mismatch"));
    auto r = std::make_shared<RecoveryRecord>();
    r->sessionId = expected;
    if (parts[2] == "open") r->disposition = RecoveryDisposition::Open;
    else if (parts[2] == "closed") r->disposition = RecoveryDisposition::Closed;
    else if (parts[2] == "rejected") r->disposition = RecoveryDisposition::Rejected;
    else throw error(ErrorCode::InvalidValue, QStringLiteral("Unknown recovery disposition"));
    auto state = decodeManifest(unbase64(parts[5]), limits.manifest);
    if (!state) throw state.error();
    auto bound = bindProjectSnapshot(std::move(state.value()), pathText(parts[3]));
    if (!bound) throw bound.error();
    auto physical = std::make_shared<SavedProject>(*bound.value());
    const auto relativeBytes = unbase64(parts[4]);
    const auto relative = QString::fromUtf8(relativeBytes);
    if (relative.toUtf8() != relativeBytes || !validateDatasetPath(relative))
        throw error(ErrorCode::InvalidReference, QStringLiteral("Unsafe recovery dataset reference"));
    physical->content.location = std::make_shared<const DatasetLocation>(DatasetLocation{
        physical->content.location->storageId, physical->content.location->storageDirectory, relative.toStdWString()});
    r->state = std::move(physical);
    if (!parts[6].isEmpty()) {
        const auto path = pathText(parts[6]);
        if (QFileInfo(path).suffix().compare(u"pc2dproj", Qt::CaseInsensitive))
            throw error(ErrorCode::InvalidReference, QStringLiteral("Invalid explicit baseline path"));
        auto snapshot = decodeManifest(unbase64(parts[7]), limits.manifest);
        if (!snapshot) throw snapshot.error();
        if (snapshot.value().projectId != r->state->snapshot.projectId || parts[8].size() != 64
            || QByteArray::fromHex(parts[8]).toHex() != parts[8])
            throw error(ErrorCode::InvalidValue, QStringLiteral("Invalid explicit baseline identity/digest"));
        auto baseline = bindProjectSnapshot(std::move(snapshot.value()), QFileInfo(path).absolutePath(),
                                            path, QByteArray::fromHex(parts[8]));
        if (!baseline) throw baseline.error();
        r->baseline = std::move(baseline.value());
    } else if (!parts[7].isEmpty() || !parts[8].isEmpty())
        throw error(ErrorCode::InvalidValue, QStringLiteral("Incomplete explicit baseline"));
    if (!legacy) {
        if (parts[9] != "0" && parts[9] != "1")
            throw error(ErrorCode::InvalidValue, QStringLiteral("Invalid recovery conflict flag"));
        r->requiresSaveAs = parts[9] == "1";
        if (r->requiresSaveAs && !r->baseline)
            throw error(ErrorCode::InvalidValue, QStringLiteral("Missing recovery conflict provenance"));
    }
    r->digest = hash(bytes);
    return r;
}
std::shared_ptr<const RecoveryRecord> readRecord(const QString &root, QUuid id, RecoveryLimits limits,
    std::uint64_t *remainingBytes = nullptr)
{
    if (remainingBytes) limits.maxRecordBytes = (std::min)(limits.maxRecordBytes, *remainingBytes);
    auto path = resolveDatasetPath(root, recoveryFileName(id));
    if (!path) throw path.error();
    if (!path.value().exists) fail(QStringLiteral("Recovery record is missing"));
    QFile file(path.value().absolutePath);
    if (!file.open(QIODevice::ReadOnly)) fail(file.errorString());
    if (!limits.maxRecordBytes || limits.maxRecordBytes >= std::uint64_t(std::numeric_limits<qint64>::max())
        || file.size() < 0 || std::uint64_t(file.size()) > limits.maxRecordBytes)
        throw error(ErrorCode::ResourceExhausted, QStringLiteral("Recovery record exceeds admission budget"));
    const auto bytes = file.read(qint64(limits.maxRecordBytes) + 1);
    if (remainingBytes) *remainingBytes -= (std::min)(*remainingBytes, std::uint64_t(bytes.size()));
    if (file.error() != QFileDevice::NoError || !file.atEnd()) fail(file.errorString());
    if (std::uint64_t(bytes.size()) > limits.maxRecordBytes)
        throw error(ErrorCode::ResourceExhausted, QStringLiteral("Recovery record grew beyond budget"));
    return decode(bytes, id, limits);
}
std::shared_ptr<const SavedProject> explicitNow(const RecoveryRecord &record, RecoveryLimits limits,
    std::uint64_t *remainingBytes = nullptr)
{
    if (!record.baseline) return {};
    const QFileInfo path(record.baseline->manifestPath);
    const auto root = path.absolutePath();
    auto resolved = resolveDatasetPath(root, path.fileName());
    if (!resolved || !resolved.value().exists) return {};
    QFile file(resolved.value().absolutePath);
    if (!file.open(QIODevice::ReadOnly)) return {};
    if (remainingBytes) limits.manifest.maxBytes = (std::min)(limits.manifest.maxBytes, *remainingBytes);
    QByteArray digest;
    auto snapshot = readManifest(file, limits.manifest, &digest);
    if (remainingBytes && file.pos() > 0) *remainingBytes -= (std::min)(*remainingBytes, std::uint64_t(file.pos()));
    if (!snapshot) {
        if (snapshot.error().code == ErrorCode::ResourceExhausted) throw snapshot.error();
        return {};
    }
    auto bound = bindProjectSnapshot(std::move(snapshot.value()), root, path.absoluteFilePath(), std::move(digest));
    if (!bound) throw bound.error();
    return bound.value();
}
bool equal(const SavedProject &a, const SavedProject &b)
{ return matchesSavedContent(ProjectState(1, a.snapshot.projectId, a.content, a.snapshot.cell), b); }
RecoveryRelation relation(const RecoveryRecord &r, const std::shared_ptr<const SavedProject> &now)
{
    if (r.requiresSaveAs) return RecoveryRelation::Conflict;
    if (!r.baseline) return RecoveryRelation::Unsaved;
    if (!now) return RecoveryRelation::Conflict;
    if (equal(*r.state, *now)) return RecoveryRelation::MatchesExplicit;
    return r.baseline->manifestDigest == now->manifestDigest ? RecoveryRelation::Modified : RecoveryRelation::Conflict;
}
Error memoryError() { return error(ErrorCode::ResourceExhausted, QStringLiteral("Recovery allocation failed")); }
} // namespace

QString recoveryFileName(QUuid id) { return id.toString(QUuid::WithoutBraces) + QStringLiteral(".pc2drecovery"); }
RecoveryLease::~RecoveryLease() { if (handle_) CloseHandle(handle_); }
Result<std::shared_ptr<const RecoveryLease>> RecoveryLease::acquire(QUuid session)
{
    if (session.isNull()) return error(ErrorCode::InvalidValue, QStringLiteral("Invalid recovery session"));
    const auto handle = CreateEventW(nullptr, TRUE, FALSE, markerName(session).toStdWString().c_str());
    const auto code = GetLastError();
    if (!handle) return error(ErrorCode::FileIO, QStringLiteral("Cannot create recovery lifetime marker (%1)").arg(code));
    if (code == ERROR_ALREADY_EXISTS) {
        CloseHandle(handle);
        return error(ErrorCode::InvalidValue, QStringLiteral("Recovery session is active"));
    }
    try {
        auto owner = std::shared_ptr<RecoveryLease>(new RecoveryLease(nullptr, session));
        owner->handle_ = handle;
        return std::shared_ptr<const RecoveryLease>(std::move(owner));
    }
    catch (...) { CloseHandle(handle); throw; }
}
Result<bool> RecoveryLease::active(QUuid session)
{
    const auto handle = OpenEventW(SYNCHRONIZE, FALSE, markerName(session).toStdWString().c_str());
    if (handle) { CloseHandle(handle); return true; }
    const auto code = GetLastError();
    if (code == ERROR_FILE_NOT_FOUND) return false;
    return error(ErrorCode::FileIO, QStringLiteral("Cannot inspect recovery lifetime marker (%1)").arg(code));
}

struct RecoveryTransaction::Impl {
    QString root;
    RecoveryLimits limits;
    std::shared_ptr<const RecoveryRecord> record;
    std::shared_ptr<const RecoveryLease> owner;
    std::unique_ptr<QLockFile> lock;
    std::unique_ptr<QSaveFile> file;
    HANDLE directory = INVALID_HANDLE_VALUE;
    std::shared_ptr<TestHooks> hooks;
    ~Impl() { file.reset(); lock.reset(); if (directory != INVALID_HANDLE_VALUE) CloseHandle(directory); }
    void step(RecoveryPhase phase) { if (hooks && hooks->phase) hooks->phase(phase); }
};
RecoveryTransaction::RecoveryTransaction(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
RecoveryTransaction::~RecoveryTransaction() = default;
const std::shared_ptr<const RecoveryRecord> &RecoveryTransaction::prepared() const noexcept { return impl_->record; }
const std::shared_ptr<const RecoveryLease> &RecoveryTransaction::lease() const noexcept { return impl_->owner; }
Result<std::unique_ptr<RecoveryTransaction>> RecoveryTransaction::prepare(QString root, RecoveryRecord record,
    std::shared_ptr<const RecoveryLease> lease, RecoveryLimits limits, std::function<bool()> cancelled)
{ return prepareImpl(std::move(root), std::move(record), std::move(lease), limits, std::move(cancelled), {}); }
Result<std::unique_ptr<RecoveryTransaction>> RecoveryTransaction::prepareImpl(QString root, RecoveryRecord record,
    std::shared_ptr<const RecoveryLease> lease, RecoveryLimits limits, std::function<bool()> cancelled,
    std::shared_ptr<TestHooks> hooks)
{
    try {
        auto p = std::make_unique<Impl>(); p->root = std::move(root); p->limits = limits; p->hooks = std::move(hooks);
        auto check = [&] { if (cancelled && cancelled())
            throw error(ErrorCode::InvalidValue, QStringLiteral("Recovery cancelled before publication")); };
        p->step(RecoveryPhase::Target); check();
        if (!absolutePath(p->root) || !QDir().mkpath(p->root)) fail(QStringLiteral("Cannot create recovery directory"));
        p->root = QFileInfo(p->root).canonicalFilePath();
        p->directory = CreateFileW(QDir::toNativeSeparators(p->root).toStdWString().c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (p->directory == INVALID_HANDLE_VALUE) fail(QStringLiteral("Cannot retain recovery directory"));
        const auto name = recoveryFileName(record.sessionId);
        auto path = resolveDatasetPath(p->root, name);
        auto lockPath = resolveDatasetPath(p->root, name + QStringLiteral(".lock"));
        if (!path) throw path.error();
        if (!lockPath) throw lockPath.error();
        p->lock = std::make_unique<QLockFile>(lockPath.value().absolutePath);
        p->lock->setStaleLockTime(0);
        if (!p->lock->tryLock(0)) fail(QStringLiteral("Recovery record is locked"));
        if (record.digest.isEmpty()) {
            if (path.value().exists) throw error(ErrorCode::InvalidValue, QStringLiteral("Recovery destination already exists"));
        } else if (readRecord(p->root, record.sessionId, limits)->digest != record.digest)
            throw error(ErrorCode::InvalidValue, QStringLiteral("Recovery record changed since discovery"));
        if (lease && lease->sessionId() != record.sessionId)
            throw error(ErrorCode::InvalidValue, QStringLiteral("Mismatched recovery owner"));
        if (!lease) {
            auto acquired = RecoveryLease::acquire(record.sessionId);
            if (!acquired) throw acquired.error();
            lease = std::move(acquired.value());
        }
        p->owner = std::move(lease);
        auto bytes = encode(record, limits);
        record.digest = hash(bytes);
        p->record = std::make_shared<const RecoveryRecord>(std::move(record));
        p->step(RecoveryPhase::Open); check();
        p->file = std::make_unique<QSaveFile>(path.value().absolutePath);
        p->file->setDirectWriteFallback(false);
        if (!p->file->open(QIODevice::WriteOnly)) fail(p->file->errorString());
        p->step(RecoveryPhase::Write); check();
        const auto count = p->hooks && p->hooks->write ? p->hooks->write(*p->file, bytes.data(), bytes.size())
                                                      : p->file->write(bytes);
        if (count != bytes.size()) fail(QStringLiteral("Short or failed recovery write"));
        p->step(RecoveryPhase::Flush); check();
        if (!(p->hooks && p->hooks->flush ? p->hooks->flush(*p->file) : p->file->flush()))
            fail(QStringLiteral("Recovery flush failed"));
        p->step(RecoveryPhase::BeforeCommit); check();
        return std::unique_ptr<RecoveryTransaction>(new RecoveryTransaction(std::move(p)));
    } catch (const Error &e) { return e; }
    catch (const std::bad_alloc &) { return memoryError(); }
}
RecoveryReceipt RecoveryTransaction::commit() noexcept
{
    RecoveryReceipt receipt;
    try {
        if (!(impl_->hooks && impl_->hooks->commit ? impl_->hooks->commit(*impl_->file) : impl_->file->commit()))
            fail(QStringLiteral("Recovery atomic publication failed"));
        receipt.published = true;
        impl_->step(RecoveryPhase::Published);
        impl_->step(RecoveryPhase::Cleanup);
    } catch (const Error &e) { if (receipt.published) receipt.cleanupWarning = true; else receipt.error = e; }
    catch (...) { if (receipt.published) receipt.cleanupWarning = true;
        else receipt.error = error(ErrorCode::FileIO, QStringLiteral("Recovery publication failed")); }
    return receipt;
}

Result<std::vector<RecoveryCandidate>> discoverRecovery(const QString &root, RecoveryLimits limits,
    cache::CacheReader::CancelCheck cancel, void *context)
{
    try {
        if (!limits.maxRecords || !limits.maxDiscoveryBytes || !absolutePath(root))
            return error(ErrorCode::InvalidReference, QStringLiteral("Invalid recovery discovery root/budget"));
        auto checkpoint = [&] { if (cancel && cancel(context))
            throw error(ErrorCode::InvalidValue, QStringLiteral("Recovery discovery cancelled")); };
        checkpoint();
        std::vector<RecoveryCandidate> found;
        const auto attributes = GetFileAttributesW(QDir::toNativeSeparators(root).toStdWString().c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            const auto code = GetLastError();
            if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) return found;
            fail(QStringLiteral("Cannot inspect recovery root (Win32 %1)").arg(code));
        }
        if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) fail(QStringLiteral("Recovery root is not a directory"));
        WIN32_FIND_DATAW data{};
        struct FindHandle { HANDLE value; ~FindHandle() { if (value != INVALID_HANDLE_VALUE) FindClose(value); } };
        FindHandle search{FindFirstFileW(QDir::toNativeSeparators(QDir(root).filePath(QStringLiteral("*.pc2drecovery")))
            .toStdWString().c_str(), &data)};
        if (search.value == INVALID_HANDLE_VALUE) {
            const auto code = GetLastError();
            if (code == ERROR_FILE_NOT_FOUND) return found;
            fail(QStringLiteral("Cannot enumerate recovery directory (Win32 %1)").arg(code));
        }
        std::uint64_t remainingBytes = limits.maxDiscoveryBytes;
        for (;;) {
            checkpoint();
            if (found.size() >= limits.maxRecords) return error(ErrorCode::ResourceExhausted, QStringLiteral("Recovery discovery record budget exhausted"));
            const auto recordBytes = (std::uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
            if (recordBytes > remainingBytes) return error(ErrorCode::ResourceExhausted, QStringLiteral("Recovery discovery byte budget exhausted"));
            const auto name = QString::fromWCharArray(data.cFileName);
            RecoveryCandidate c; c.sessionId = QUuid(QFileInfo(name).completeBaseName());
            try {
                if (c.sessionId.isNull() || name != recoveryFileName(c.sessionId))
                    throw error(ErrorCode::InvalidReference, QStringLiteral("Invalid recovery filename"));
                c.record = readRecord(root, c.sessionId, limits, &remainingBytes);
                auto active = RecoveryLease::active(c.sessionId);
                if (!active) throw active.error();
                c.session = active.value() ? RecoverySessionState::Active
                    : c.record->disposition == RecoveryDisposition::Closed ? RecoverySessionState::Closed
                    : c.record->disposition == RecoveryDisposition::Rejected ? RecoverySessionState::Rejected
                                                                          : RecoverySessionState::Interrupted;
                checkpoint();
                c.relation = relation(*c.record, explicitNow(*c.record, limits, &remainingBytes));
            } catch (const Error &e) { c.error = e; c.session = RecoverySessionState::Invalid; }
            found.push_back(std::move(c));
            if (!FindNextFileW(search.value, &data)) {
                const auto code = GetLastError();
                if (code != ERROR_NO_MORE_FILES) fail(QStringLiteral("Recovery enumeration failed (Win32 %1)").arg(code));
                break;
            }
        }
        return found;
    } catch (const Error &e) { return e; }
    catch (const std::bad_alloc &) { return memoryError(); }
}
Result<RestoredRecovery> restoreRecovery(const QString &root, const RecoveryCandidate &candidate,
    RecoveryLimits limits, cache::ResourceLimits readerLimits, cache::CacheReader::CancelCheck cancel, void *context)
{
    try {
        auto lease = RecoveryLease::acquire(candidate.sessionId);
        if (!lease) return lease.error();
        auto r = readRecord(root, candidate.sessionId, limits);
        if (!candidate.record || r->digest != candidate.record->digest || r->disposition == RecoveryDisposition::Rejected)
            return error(ErrorCode::InvalidValue, QStringLiteral("Stale or rejected recovery candidate"));
        auto now = explicitNow(*r, limits);
        const auto kind = relation(*r, now);
        auto state = std::make_shared<SavedProject>(*r->state);
        if ((kind == RecoveryRelation::MatchesExplicit || kind == RecoveryRelation::Modified)
            && now->content.cacheId == state->content.cacheId) {
            state->content.location = now->content.location;
        }
        auto loaded = recheckPointCache(*state, readerLimits, cancel, context);
        if (!loaded) return loaded.error();
        std::shared_ptr<const SavedProject> baseline;
        if (kind == RecoveryRelation::MatchesExplicit || kind == RecoveryRelation::Modified) {
            auto observed = std::make_shared<SavedProject>(*now);
            observed->datasetIdentity = loaded.value().saved->datasetIdentity;
            baseline = std::move(observed);
        }
        if (cancel && cancel(context)) return error(ErrorCode::InvalidValue, QStringLiteral("Recovery acceptance cancelled"));
        if (r->disposition == RecoveryDisposition::Closed || (kind == RecoveryRelation::Conflict && !r->requiresSaveAs)) {
            // Persist the new attempt before exposing its candidate. Even a crash
            // immediately after owner Accept leaves an interrupted, complete record.
            // Only lifecycle/conflict metadata changes; state and references stay.
            auto reopened = *r; reopened.disposition = RecoveryDisposition::Open;
            reopened.requiresSaveAs = kind == RecoveryRelation::Conflict;
            auto transaction = RecoveryTransaction::prepare(root, std::move(reopened), lease.value(), limits,
                [cancel, context] { return cancel && cancel(context); });
            if (!transaction) return transaction.error();
            const auto receipt = transaction.value()->commit();
            if (!receipt.published) return *receipt.error;
            r = transaction.value()->prepared();
        }
        return RestoredRecovery{std::move(loaded.value()), std::move(baseline), std::move(r),
                                std::move(lease.value()), kind == RecoveryRelation::Conflict};
    } catch (const Error &e) { return e; }
    catch (const std::bad_alloc &) { return memoryError(); }
}
Status rejectRecovery(const QString &root, const RecoveryCandidate &candidate, RecoveryLimits limits)
{
    if (!candidate.record) return error(ErrorCode::InvalidValue, QStringLiteral("Invalid recovery candidate"));
    auto record = *candidate.record; record.disposition = RecoveryDisposition::Rejected;
    auto transaction = RecoveryTransaction::prepare(root, std::move(record), {}, limits);
    if (!transaction) return transaction.error();
    const auto receipt = transaction.value()->commit();
    return receipt.published ? Status(std::monostate{}) : Status(*receipt.error);
}
Status closeRecovery(const QString &root, QUuid session, RecoveryLimits limits,
    std::shared_ptr<const RecoveryLease> lease, bool discardConfirmed)
{
    try {
        const auto attributes = GetFileAttributesW(QDir::toNativeSeparators(QDir(root).filePath(recoveryFileName(session)))
            .toStdWString().c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            const auto code = GetLastError();
            if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) return std::monostate{};
            fail(QStringLiteral("Cannot inspect closing recovery record (Win32 %1)").arg(code));
        }
        auto record = *readRecord(root, session, limits);
        if (record.disposition == RecoveryDisposition::Rejected) return std::monostate{};
        if (record.disposition == RecoveryDisposition::Closed && !discardConfirmed) return std::monostate{};
        record.disposition = discardConfirmed ? RecoveryDisposition::Rejected : RecoveryDisposition::Closed;
        auto transaction = RecoveryTransaction::prepare(root, std::move(record), std::move(lease), limits);
        if (!transaction) return transaction.error();
        const auto receipt = transaction.value()->commit();
        return receipt.published ? Status(std::monostate{}) : Status(*receipt.error);
    } catch (const Error &e) { return e; }
    catch (const std::bad_alloc &) { return memoryError(); }
}
} // namespace cadcontour::persistence
