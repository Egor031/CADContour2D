#include "project_open.h"

#include <QDir>
#include <QFileInfo>

#define NOMINMAX
#include <Windows.h>

#include <bit>

namespace cadcontour::persistence {
namespace {
Error ioError(const QString &detail) { return {ErrorCode::FileIO, {}, detail.left(256)}; }
struct ReadLease {
    HANDLE handle = INVALID_HANDLE_VALUE;
    ~ReadLease() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    std::optional<DatasetFileIdentity> identity() const
    {
        BY_HANDLE_FILE_INFORMATION info{};
        FILE_BASIC_INFO basic{};
        if (!GetFileInformationByHandle(handle, &info)
            || !GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic))
            || (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) return {};
        return DatasetFileIdentity{info.dwVolumeSerialNumber,
            (std::uint64_t(info.nFileIndexHigh) << 32) | info.nFileIndexLow,
            (std::uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow,
            std::uint64_t(basic.LastWriteTime.QuadPart), std::uint64_t(basic.ChangeTime.QuadPart)};
    }
};
bool sameMetadata(const cache::CacheMetadata &a, const cache::CacheMetadata &b)
{
    auto bits = [](double x) { return std::bit_cast<std::uint64_t>(x); };
    return a.id == b.id && a.pointCount == b.pointCount && a.hasNonZeroZ == b.hasNonZeroZ
        && bits(a.bounds.minX) == bits(b.bounds.minX) && bits(a.bounds.minY) == bits(b.bounds.minY)
        && bits(a.bounds.maxX) == bits(b.bounds.maxX) && bits(a.bounds.maxY) == bits(b.bounds.maxY);
}
Result<LoadedProject> probe(std::shared_ptr<SavedProject> saved, cache::ResourceLimits limits,
                           cache::CacheReader::CancelCheck cancel, void *context)
{
    if (limits.maxOpenFiles < 2 || !limits.memoryBytes)
        return Error{ErrorCode::ResourceExhausted, {}, QStringLiteral("Limited dataset check requires a reader budget and two file handles")};
    auto availability = std::make_shared<CacheAvailability>();
    saved->datasetIdentity.reset();
    const auto &descriptor = saved->snapshot.datasets.at(0);
    const auto root = QString::fromStdWString(saved->content.location->storageDirectory.wstring());
    auto resolved = resolveDatasetPath(root, descriptor.relativePath);
    if (!resolved) {
        availability->access = resolved.error().code == ErrorCode::FileIO ? CacheAccess::FileIO : CacheAccess::UnsafePath;
        availability->pathError = resolved.error();
    } else if (!resolved.value().exists) {
        availability->access = CacheAccess::Missing;
    } else {
        // Deny ordinary write/delete while observing identity and reading the envelope/root.
        ReadLease lease;
        lease.handle = CreateFileW(QDir::toNativeSeparators(resolved.value().absolutePath).toStdWString().c_str(),
            GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        const auto openError = GetLastError();
        const auto identity = lease.identity();
        if (lease.handle == INVALID_HANDLE_VALUE || !identity) {
            availability->access = CacheAccess::FileIO;
            availability->pathError = ioError(QStringLiteral("Cannot lease dataset (Win32 %1)")
                .arg(lease.handle == INVALID_HANDLE_VALUE ? openError : GetLastError()));
        } else {
            // The resolver's observation is repeated immediately before path-based Reader I/O.
            resolved = resolveDatasetPath(root, descriptor.relativePath);
            if (!resolved || !resolved.value().exists) {
                availability->access = CacheAccess::UnsafePath;
                availability->pathError = resolved ? ioError(QStringLiteral("Dataset disappeared")) : resolved.error();
            } else {
                auto reader = cache::CacheReader::open(resolved.value().absolutePath, limits, cancel, context);
                if (!reader) {
                    const auto &e = reader.error();
                    if (e.code == cache::ErrorCode::ResourceExhausted || e.code == cache::ErrorCode::InvalidInput)
                        return Error{ErrorCode::ResourceExhausted, {}, e.detail};
                    availability->cacheError = e;
                    availability->access = e.code == cache::ErrorCode::UnsupportedFormat ? CacheAccess::Unsupported
                        : e.code == cache::ErrorCode::CorruptCache || e.code == cache::ErrorCode::IncompleteCache
                            ? CacheAccess::Corrupt : CacheAccess::FileIO;
                } else if (!sameMetadata(reader.value()->metadata(), descriptor.metadata)
                           || identity != lease.identity()) {
                    availability->access = CacheAccess::Corrupt;
                    availability->cacheError = cache::makeError(cache::ErrorCode::CorruptCache,
                        cache::Phase::CacheRead, cache::Operation::Decode, QStringLiteral("Dataset metadata differs from manifest"));
                } else {
                    availability->reads = reader.value()->statistics();
                    saved->datasetIdentity = identity;
                }
            }
        }
    }
    return LoadedProject{std::move(saved), std::move(availability)};
}
} // namespace

Result<LoadedProject> loadProject(const QString &path, ManifestLimits limits,
    cache::ResourceLimits readerLimits, cache::CacheReader::CancelCheck cancel, void *context)
{
    const QFileInfo selected(path);
    if (!selected.isAbsolute() || selected.suffix().compare(u"pc2dproj", Qt::CaseInsensitive) != 0
        || !validateDatasetPath(selected.fileName()))
        return Error{ErrorCode::InvalidReference, {}, QStringLiteral("Expected an absolute .pc2dproj path")};
    const auto root = QFileInfo(selected.absolutePath()).canonicalFilePath();
    auto manifest = resolveDatasetPath(root, selected.fileName());
    if (!manifest) return manifest.error();
    if (!manifest.value().exists) return ioError(QStringLiteral("Manifest is missing"));
    QFile file(manifest.value().absolutePath);
    if (!file.open(QIODevice::ReadOnly)) return ioError(file.errorString());
    QByteArray digest;
    auto snapshot = readManifest(file, limits, &digest);
    if (!snapshot) return snapshot.error();
    file.close(); // dataset probe's two-handle budget excludes this completed metadata read
    auto saved = std::make_shared<SavedProject>();
    saved->snapshot = std::move(snapshot.value());
    saved->manifestDigest = std::move(digest);
    saved->manifestPath = manifest.value().absolutePath;
    const auto &s = saved->snapshot;
    const auto &d = s.datasets.at(0);
    saved->content = ImportedDataset{d.metadata.id,
        std::make_shared<const DatasetLocation>(DatasetLocation{
            s.storageId.toString(QUuid::WithoutBraces).toStdString(), root.toStdWString(), d.relativePath.toStdWString()}),
        s.source.path.toStdWString(), s.source.bytes, s.source.lines, d.metadata.pointCount,
        {d.metadata.bounds.minX, d.metadata.bounds.minY}, {d.metadata.bounds.maxX, d.metadata.bounds.maxY}, d.metadata.hasNonZeroZ};
    return probe(std::move(saved), readerLimits, cancel, context);
}

Result<LoadedProject> recheckPointCache(const SavedProject &saved, cache::ResourceLimits limits,
    cache::CacheReader::CancelCheck cancel, void *context)
{
    return probe(std::make_shared<SavedProject>(saved), limits, cancel, context);
}
} // namespace cadcontour::persistence
