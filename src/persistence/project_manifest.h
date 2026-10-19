#pragma once

#include "core/project_state.h"
#include "cache/point_cache_format.h"

#include <QByteArray>
#include <QIODevice>
#include <QUuid>

#include <variant>
#include <vector>

namespace cadcontour::persistence {

enum class ErrorCode {
    InvalidJson, UnsupportedVersion, UnsupportedFormat, MissingField, InvalidType,
    NumericRange, InvalidValue, InconsistentState, InvalidReference, ResourceExhausted, FileIO
};
struct Error {
    ErrorCode code;
    QString field;
    QString detail;
};
template<class T> class Result {
public:
    Result(T value) : data_(std::move(value)) {}
    Result(Error error) : data_(std::move(error)) {}
    explicit operator bool() const noexcept { return std::holds_alternative<T>(data_); }
    T &value() { return std::get<T>(data_); }
    const T &value() const { return std::get<T>(data_); }
    const Error &error() const { return std::get<Error>(data_); }
private:
    std::variant<T, Error> data_;
};
using Status = Result<std::monostate>;

// Caller-selected metadata admission budgets; zero is never unlimited. No version count cap.
// Tokens include property names, scalars and opening containers. Not a process RSS limit.
struct ManifestLimits {
    std::uint64_t maxBytes;
    std::uint64_t maxTokens;
    std::uint32_t maxDepth;
};
struct SourceMetadata {
    QString path; // informational only; never resolved as an internal dataset
    std::uint64_t bytes = 0;
    std::uint64_t lines = 0;
};
struct DatasetDescriptor {
    QString relativePath;
    cache::CacheMetadata metadata; // v1 point cache; new kinds require a schema version
};
struct ProjectSnapshot {
    ProjectId projectId;
    QUuid storageId;
    QUuid snapshotId;
    double cell = 1;
    SourceMetadata source;
    cache::CacheId pointCacheId;
    std::vector<DatasetDescriptor> datasets;
};

// Metadata-only projection for a prospective destination. Does not save, copy, read a
// dataset, establish a save baseline or assert the destination files are available.
Result<ProjectSnapshot> captureSnapshot(const ProjectState &state, QUuid storageId, QUuid snapshotId);
Result<QByteArray> encodeManifest(const ProjectSnapshot &snapshot, ManifestLimits limits);
Result<ProjectSnapshot> decodeManifest(const QByteArray &utf8, ManifestLimits limits);
// Bounded read from an already opened metadata device; no dataset I/O.
Result<ProjectSnapshot> readManifest(QIODevice &device, ManifestLimits limits);

} // namespace cadcontour::persistence
