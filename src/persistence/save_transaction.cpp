#include "save_transaction.h"
#include "cache/cache_file.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QSaveFile>
#include <QStorageInfo>
#include <QTemporaryFile>

#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <stdexcept>

namespace cadcontour::persistence {
namespace {
SaveError error(SaveErrorCode code, SavePhase phase, const char *detail, DWORD system = 0) noexcept
{
    SaveError e{code, phase, system};
    if (detail) {
        std::size_t i = 0;
        for (; i + 1 < e.detail.size() && detail[i]; ++i) e.detail[i] = detail[i];
    }
    return e;
}
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    void open(const QString &path, DWORD access, DWORD share, DWORD flags)
    {
        value = CreateFileW(QDir::toNativeSeparators(path).toStdWString().c_str(), access, share,
                            nullptr, OPEN_EXISTING, flags | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (value == INVALID_HANDLE_VALUE) throw GetLastError();
        FILE_ATTRIBUTE_TAG_INFO tag{};
        if (!GetFileInformationByHandleEx(value, FileAttributeTagInfo, &tag, sizeof(tag))) throw GetLastError();
        if (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) throw DWORD(ERROR_REPARSE_TAG_INVALID);
    }
    DatasetFileIdentity identity() const
    {
        BY_HANDLE_FILE_INFORMATION i{};
        FILE_BASIC_INFO basic{};
        if (!GetFileInformationByHandle(value, &i)
            || !GetFileInformationByHandleEx(value, FileBasicInfo, &basic, sizeof(basic))) throw GetLastError();
        if (i.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) throw DWORD(ERROR_INVALID_DATA);
        return {i.dwVolumeSerialNumber, (std::uint64_t(i.nFileIndexHigh) << 32) | i.nFileIndexLow,
                (std::uint64_t(i.nFileSizeHigh) << 32) | i.nFileSizeLow,
                std::uint64_t(basic.LastWriteTime.QuadPart), std::uint64_t(basic.ChangeTime.QuadPart)};
    }
    void read(std::uint64_t offset, char *buffer, DWORD count)
    {
        LARGE_INTEGER position; position.QuadPart = static_cast<LONGLONG>(offset);
        if (!SetFilePointerEx(value, position, nullptr, FILE_BEGIN)) throw GetLastError();
        DWORD received = 0;
        if (!ReadFile(value, buffer, count, &received, nullptr)) throw GetLastError();
        if (received != count) throw DWORD(ERROR_HANDLE_EOF);
    }
};
bool same(double a, double b) noexcept { return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b); }
bool sameMetadata(const cache::CacheMetadata &a, const cache::CacheMetadata &b) noexcept
{
    return a.id == b.id && a.pointCount == b.pointCount && a.hasNonZeroZ == b.hasNonZeroZ
        && same(a.bounds.minX, b.bounds.minX) && same(a.bounds.minY, b.bounds.minY)
        && same(a.bounds.maxX, b.bounds.maxX) && same(a.bounds.maxY, b.bounds.maxY);
}
bool equivalent(const QString &a, const QString &b) { return a.compare(b, Qt::CaseInsensitive) == 0; }
} // namespace

bool matchesSavedContent(const ProjectState &state, const SavedProject &saved) noexcept
{
    const auto *a = state.dataset();
    const auto &b = saved.content;
    return a && state.projectId() == saved.snapshot.projectId && same(state.cell(), saved.snapshot.cell)
        && a->cacheId == b.cacheId && a->sourcePath == b.sourcePath
        && a->sourceBytes == b.sourceBytes && a->sourceLines == b.sourceLines
        && a->pointCount == b.pointCount && a->hasNonZeroZ == b.hasNonZeroZ
        && same(a->minimum.x, b.minimum.x) && same(a->minimum.y, b.minimum.y)
        && same(a->maximum.x, b.maximum.x) && same(a->maximum.y, b.maximum.y);
}

struct SaveTransaction::Impl {
    SaveInput input;
    SaveCallbacks callbacks;
    std::shared_ptr<TestHooks> hooks;
    SavePhase phase = SavePhase::Target;
    QString root, target, relative;
    std::unique_ptr<QLockFile> lock;
    std::vector<std::unique_ptr<Handle>> directories;
    Handle source, destination;
    std::unique_ptr<QSaveFile> manifest;
    std::unique_ptr<QTemporaryFile> newManifest;
    std::shared_ptr<const SavedProject> result;
    QByteArray encoded;
    bool reuse = false;
    bool commitStarted = false;

    void step(SavePhase p, bool cancellable = true)
    {
        phase = p;
        if (hooks && hooks->phase) hooks->phase(p);
        if (cancellable && callbacks.cancelled && callbacks.cancelled())
            throw error(SaveErrorCode::Cancelled, phase, "Save cancelled before publication permission");
    }
    [[noreturn]] void fail(SaveErrorCode code, const char *detail) const { throw error(code, phase, detail); }
    void retainDirectory(const QString &path)
    {
        auto h = std::make_unique<Handle>();
        h->open(path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_FLAG_BACKUP_SEMANTICS);
        directories.push_back(std::move(h)); // deny rename/delete until transaction ends
    }
    QString resolve(const QString &path, bool mustExist = true)
    {
        auto r = resolveDatasetPath(root, path);
        if (!r || (mustExist && !r.value().exists)) fail(SaveErrorCode::InvalidTarget, "Unsafe or absent project-owned path");
        return r.value().absolutePath;
    }
    void checkTarget()
    {
        resolve(QFileInfo(target).fileName(), false);
        const QFileInfo info(target);
        if (reuse) {
            QFile file(target);
            if (!file.open(QIODevice::ReadOnly)) fail(SaveErrorCode::Conflict, "Published manifest is unavailable");
            auto parsed = readManifest(file, input.limits);
            if (!parsed || parsed.value().projectId != input.previous->snapshot.projectId
                || parsed.value().storageId != input.previous->snapshot.storageId
                || parsed.value().snapshotId != input.previous->snapshot.snapshotId)
                fail(SaveErrorCode::Conflict, "Published snapshot differs from expected snapshotId");
            if (!file.seek(0)) fail(SaveErrorCode::FileIO, "Cannot reread manifest");
            QCryptographicHash digest(QCryptographicHash::Sha256);
            std::uint64_t read = 0;
            while (!file.atEnd()) {
                const auto bytes = file.read(16384);
                if (bytes.isEmpty() || std::uint64_t(bytes.size()) > input.limits.maxBytes - read)
                    fail(SaveErrorCode::Conflict, "Published manifest size changed while reading");
                read += std::uint64_t(bytes.size());
                digest.addData(bytes);
            }
            if (file.error() != QFileDevice::NoError || digest.result() != input.previous->manifestDigest)
                fail(SaveErrorCode::Conflict, "Published manifest content changed");
        } else if (info.exists()) fail(SaveErrorCode::Conflict, "Destination already exists");
        const auto manifests = QDir(root).entryList({QStringLiteral("*.pc2dproj")}, QDir::Files | QDir::Hidden | QDir::System);
        for (const auto &name : manifests)
            if (!reuse || !equivalent(name, info.fileName())) fail(SaveErrorCode::Conflict, "Directory contains another project manifest");
    }
    cache::FileLayout envelope(Handle &file, const cache::CacheMetadata &expected)
    {
        const auto identity = file.identity();
        std::array<char, cache::format::headerSize> bytes;
        file.read(0, bytes.data(), DWORD(bytes.size()));
        const auto span = cache::Bytes{reinterpret_cast<const std::byte *>(bytes.data()), bytes.size()};
        auto header = cache::decodeHeader(span);
        if (!header || !sameMetadata(header.value().metadata, expected)) fail(SaveErrorCode::ChangedDataset, "Cache header or identity differs from snapshot");
        auto layout = cache::calculateLayout(expected.pointCount, header.value().index.pageCount);
        if (!layout || layout.value().fileSize != identity.size) fail(SaveErrorCode::ChangedDataset, "Cache size differs from completed layout");
        file.read(layout.value().footerOffset, bytes.data(), DWORD(bytes.size()));
        auto footer = cache::decodeFooter(span);
        if (!footer || !cache::validateEnvelope(header.value(), footer.value(), identity.size))
            fail(SaveErrorCode::ChangedDataset, "Cache completion footer is invalid");
        return layout.value();
    }
    void write(QIODevice &file, const char *bytes, qint64 size)
    {
        const auto written = hooks && hooks->write ? hooks->write(file, bytes, size) : file.write(bytes, size);
        if (written != size) fail(SaveErrorCode::FileIO, "Short or failed file write");
    }
    void copy(const cache::FileLayout &layout)
    {
        step(SavePhase::Staging);
        const auto datasetPath = resolve(relative, false);
        const auto parent = QFileInfo(datasetPath).absolutePath();
        // Resolve before and after creation; retain each descendant directory against rename.
        auto current = root;
        const auto parts = relative.split(u'/');
        for (qsizetype i = 0; i + 1 < parts.size(); ++i) {
            current += u'/' + parts[i];
            if (!QDir().mkpath(current)) fail(SaveErrorCode::FileIO, "Cannot create dataset directory");
            resolve(relative, false);
            retainDirectory(current);
        }
        QStorageInfo storage(parent);
        if (!storage.isValid() || !storage.isReady() || storage.bytesAvailable() < 0
            || std::uint64_t(storage.bytesAvailable()) < layout.fileSize)
            fail(SaveErrorCode::ResourceUnavailable, "Insufficient available destination storage");
        QTemporaryFile staging(parent + QStringLiteral("/.save-XXXXXX"));
        if (!staging.open()) fail(SaveErrorCode::FileIO, "Cannot create exclusive dataset staging file");
        std::array<char, cache::format::pageSize> buffer;
        QCryptographicHash original(QCryptographicHash::Sha256);
        for (std::uint64_t offset = 0; offset < layout.fileSize;) {
            step(SavePhase::CopyRead);
            const auto count = DWORD(std::min<std::uint64_t>(buffer.size(), layout.fileSize - offset));
            source.read(offset, buffer.data(), count);
            original.addData(QByteArrayView(buffer.data(), count));
            step(SavePhase::CopyWrite);
            write(staging, buffer.data(), count);
            offset += count;
            if (callbacks.progress) callbacks.progress(offset, layout.fileSize);
        }
        if (!staging.flush() || staging.size() != qint64(layout.fileSize)) fail(SaveErrorCode::FileIO, "Cannot flush completed dataset");
        step(SavePhase::CopyVerify);
        if (!staging.seek(0)) fail(SaveErrorCode::FileIO, "Cannot seek copied dataset");
        QCryptographicHash copied(QCryptographicHash::Sha256);
        for (std::uint64_t offset = 0; offset < layout.fileSize;) {
            step(SavePhase::CopyVerify);
            const auto count = qint64(offset == 0 || offset == layout.footerOffset
                ? cache::format::headerSize : cache::format::pageSize);
            if (staging.read(buffer.data(), count) != count) fail(SaveErrorCode::FileIO, "Cannot read back copied dataset");
            copied.addData(QByteArrayView(buffer.data(), count));
            if (offset >= cache::format::headerSize && offset < layout.footerOffset
                && !cache::validatePage({reinterpret_cast<const std::byte *>(buffer.data()), std::size_t(count)}))
                fail(SaveErrorCode::ChangedDataset, "Copied cache page is corrupt");
            offset += std::uint64_t(count);
        }
        if (original.result() != copied.result()) fail(SaveErrorCode::ChangedDataset, "Dataset copy digest mismatch");
        step(SavePhase::DatasetPublish);
        resolve(relative, false);
        if (!staging.rename(datasetPath)) fail(SaveErrorCode::Conflict, "Cannot publish dataset without overwrite");
        staging.setAutoRemove(false); // no fallible action between rename and retention
        step(SavePhase::DatasetPublished);
    }
    void prepare()
    {
        step(SavePhase::Target);
        const QFileInfo requested(input.targetPath);
        if (!requested.isAbsolute() || requested.suffix().compare(u"pc2dproj", Qt::CaseInsensitive) != 0
            || !validateDatasetPath(requested.fileName())) fail(SaveErrorCode::InvalidTarget, "Expected absolute .pc2dproj path");
        if (!QDir().mkpath(requested.absolutePath())) fail(SaveErrorCode::FileIO, "Cannot create project directory");
        root = QFileInfo(requested.absolutePath()).canonicalFilePath();
        if (root.isEmpty()) fail(SaveErrorCode::InvalidTarget, "Cannot resolve project directory");
        retainDirectory(root);
        target = QDir(root).filePath(requested.fileName());
        lock = std::make_unique<QLockFile>(QDir(root).filePath(QStringLiteral(".pc2d-save.lock")));
        lock->setStaleLockTime(0); // no age-based stealing; Qt still recognizes dead local owners
        if (!lock->tryLock(0)) fail(SaveErrorCode::Locked, "Project directory is locked by another save");
        if (input.previous) {
            const auto prior = QFileInfo(input.previous->manifestPath).absolutePath();
            const auto canonicalPrior = QFileInfo(prior).canonicalFilePath();
            if (equivalent(root, canonicalPrior)) {
                if (!equivalent(QFileInfo(target).fileName(), QFileInfo(input.previous->manifestPath).fileName()))
                    fail(SaveErrorCode::Conflict, "Save As in the same directory requires the current manifest name");
                reuse = true;
            }
            if (!input.saveAs && !reuse) fail(SaveErrorCode::InvalidTarget, "Use Save As to change the destination");
        }
        checkTarget();
        if (!reuse && QFileInfo::exists(QDir(root).filePath(QStringLiteral("data"))))
            fail(SaveErrorCode::Conflict, "New project destination already contains data");
        if (reuse) input.snapshot.storageId = input.previous->snapshot.storageId;
        relative = pointCacheRelativePath(input.snapshot.storageId, input.snapshot.pointCacheId);
        input.snapshot.datasets.at(0).relativePath = relative;
        step(SavePhase::Source);
        if (!input.dataset.location) fail(SaveErrorCode::MissingDataset, "Snapshot has no dataset binding");
        const auto directory = QString::fromStdWString(input.dataset.location->storageDirectory.wstring());
        const auto sourceRelative = QString::fromStdWString(input.dataset.location->relativeCachePath.generic_wstring());
        auto resolvedSource = resolveDatasetPath(directory, sourceRelative);
        if (!resolvedSource || !resolvedSource.value().exists) fail(SaveErrorCode::MissingDataset, "Snapshot dataset is unavailable or unsafe");
        source.open(resolvedSource.value().absolutePath, GENERIC_READ, FILE_SHARE_READ, FILE_ATTRIBUTE_NORMAL);
        const auto identity = source.identity();
        const auto layout = envelope(source, input.snapshot.datasets.at(0).metadata);
        if (input.previous && identity != input.previous->datasetIdentity)
            fail(SaveErrorCode::ChangedDataset, "Previously saved immutable source cache changed");
        if (reuse) {
            if (!equivalent(resolve(relative), resolvedSource.value().absolutePath)
                || identity != input.previous->datasetIdentity)
                fail(SaveErrorCode::ChangedDataset, "Saved immutable cache file changed");
            auto current = root;
            const auto parts = relative.split(u'/');
            for (qsizetype i = 0; i + 1 < parts.size(); ++i) {
                current += u'/' + parts[i];
                retainDirectory(current);
            }
        } else copy(layout);
        step(SavePhase::ManifestPrepare);
        destination.open(resolve(relative), GENERIC_READ, FILE_SHARE_READ, FILE_ATTRIBUTE_NORMAL);
        envelope(destination, input.snapshot.datasets.at(0).metadata);
        auto prepared = std::make_shared<SavedProject>();
        prepared->snapshot = input.snapshot;
        prepared->content = input.dataset;
        prepared->content.location = std::make_shared<const DatasetLocation>(DatasetLocation{
            input.snapshot.storageId.toString(QUuid::WithoutBraces).toStdString(), root.toStdWString(), relative.toStdWString()});
        prepared->manifestPath = target;
        prepared->datasetIdentity = destination.identity();
        auto bytes = encodeManifest(input.snapshot, input.limits);
        if (!bytes) fail(SaveErrorCode::InvalidSnapshot, "Cannot encode snapshot with selected manifest limits");
        encoded = std::move(bytes.value());
        prepared->manifestDigest = QCryptographicHash::hash(encoded, QCryptographicHash::Sha256);
        result = std::move(prepared);
        step(SavePhase::ManifestWrite);
        checkTarget();
        QFileDevice *device;
        if (reuse) {
            manifest = std::make_unique<QSaveFile>(target);
            manifest->setDirectWriteFallback(false);
            if (!manifest->open(QIODevice::WriteOnly)) fail(SaveErrorCode::FileIO, "Cannot open atomic manifest staging file");
            device = manifest.get();
        } else {
            newManifest = std::make_unique<QTemporaryFile>(root + QStringLiteral("/.save-manifest-XXXXXX"));
            if (!newManifest->open()) fail(SaveErrorCode::FileIO, "Cannot create exclusive manifest staging file");
            device = newManifest.get();
        }
        write(*device, encoded.constData(), encoded.size());
        if (!device->flush()) fail(SaveErrorCode::FileIO, "Cannot flush manifest staging file");
        callbacks = {}; // prepare-only borrowed WorkerControl is no longer needed
    }
};

SaveTransaction::SaveTransaction(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
SaveTransaction::~SaveTransaction() = default;
const std::shared_ptr<const SavedProject> &SaveTransaction::prepared() const noexcept { return impl_->result; }
SaveResult<std::unique_ptr<SaveTransaction>> SaveTransaction::prepare(SaveInput input, SaveCallbacks callbacks)
{
    return prepareImpl(std::move(input), std::move(callbacks), {});
}
SaveResult<std::unique_ptr<SaveTransaction>> SaveTransaction::prepareImpl(
    SaveInput input, SaveCallbacks callbacks, std::shared_ptr<TestHooks> hooks)
{
    std::unique_ptr<Impl> impl;
    try {
        impl = std::make_unique<Impl>();
        impl->input = std::move(input); impl->callbacks = std::move(callbacks); impl->hooks = std::move(hooks);
        impl->prepare();
        return std::unique_ptr<SaveTransaction>(new SaveTransaction(std::move(impl)));
    } catch (SaveError e) { return e; }
    catch (DWORD code) { return error(SaveErrorCode::FileIO, impl ? impl->phase : SavePhase::Target, "Windows file operation failed", code); }
    catch (const std::bad_alloc &) { return error(SaveErrorCode::ResourceUnavailable, impl ? impl->phase : SavePhase::Target, "Save allocation failed"); }
    catch (const std::exception &e) { return error(SaveErrorCode::FileIO, impl ? impl->phase : SavePhase::Target, e.what()); }
    catch (...) { return error(SaveErrorCode::FileIO, impl ? impl->phase : SavePhase::Target, "Unexpected save preparation failure"); }
}
SaveCommitResult SaveTransaction::commit() noexcept
{
    auto &p = *impl_;
    SaveCommitResult receipt;
    try {
        p.step(SavePhase::BeforeCommit, false);
        p.checkTarget(); // lock remains held across both this check and commit
        p.resolve(p.relative);
        if (p.destination.identity() != p.result->datasetIdentity)
            p.fail(SaveErrorCode::ChangedDataset, "Prepared dataset changed before manifest commit");
        p.step(SavePhase::ManifestCommit, false);
        if (p.reuse) {
            p.commitStarted = true;
            const bool ok = p.hooks && p.hooks->commit ? p.hooks->commit(*p.manifest) : p.manifest->commit();
            if (!ok) {
                // Replacement did not confirm its outcome: keep every final dataset
                // and prepared evidence; never advance the application's baseline.
                receipt.publication = SavePublication::Uncertain;
                receipt.error = error(SaveErrorCode::PublicationUncertain, p.phase, "Manifest commit did not confirm publication");
                return receipt;
            }
        } else {
            // Same-directory no-overwrite rename also protects a previously absent
            // target from a non-cooperating creator after the final conflict check.
            if (!p.newManifest->rename(p.target))
                p.fail(SaveErrorCode::Conflict, "Cannot publish new manifest without overwrite");
            p.newManifest->setAutoRemove(false);
        }
        receipt.publication = SavePublication::Persisted;
    } catch (SaveError e) { receipt.error = e; }
    catch (DWORD code) { receipt.error = error(SaveErrorCode::FileIO, p.phase, "Windows publication check failed", code); }
    catch (const std::exception &e) { receipt.error = error(SaveErrorCode::FileIO, p.phase, e.what()); }
    catch (...) { receipt.error = error(SaveErrorCode::FileIO, p.phase, "Unexpected manifest commit failure"); }
    if (receipt.publication != SavePublication::Persisted && p.commitStarted) {
        receipt.publication = SavePublication::Uncertain;
        if (receipt.error) receipt.error->code = SaveErrorCode::PublicationUncertain;
    }
    if (receipt.publication == SavePublication::Persisted) {
        try { p.step(SavePhase::Receipt, false); } catch (...) { receipt.cleanupWarning = true; }
        try { p.step(SavePhase::Cleanup, false); } catch (...) { receipt.cleanupWarning = true; }
    }
    return receipt;
}

} // namespace cadcontour::persistence
