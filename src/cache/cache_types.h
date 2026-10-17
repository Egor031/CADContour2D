#pragma once

#include "core/coordinates.h"

#include <QString>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <variant>

namespace cadcontour::cache {

using RecordId = std::uint64_t;

struct CacheId {
    std::array<std::byte, 16> bytes{}; // UUID in RFC 4122 byte order, never native QUuid memory
    static CacheId create();
    bool isNull() const noexcept;
    bool operator==(const CacheId &) const = default;
};

struct PointRecord {
    RecordId id = 0;
    ModelPoint point;
};

struct Measurement { double x = 0; double y = 0; double z = 0; };

struct BoundingBox {
    double minX = 0;
    double minY = 0;
    double maxX = 0;
    double maxY = 0;
};

bool validPoint(ModelPoint point) noexcept;
bool validBounds(BoundingBox bounds) noexcept;

enum class ErrorCode {
    InvalidInput, UnsupportedFormat, CorruptCache, IncompleteCache,
    OpenFailed, ReadFailed, WriteFailed, SeekFailed, FlushFailed,
    PublishFailed, ResourceExhausted, Cancelled, InvalidState, ConsumerFailed, CleanupFailed,
    ParseError, NumericRange, InsufficientData, SourceChanged
};
enum class Phase { Codec, Layout, FileIO, Publication, Ingest, RunSort, RunWrite, Merge, Output, Cleanup,
                   PointPages, IndexBuild, Sealing, CacheRead, Validation, SourceRead, Parse };
enum class Operation { Encode, Decode, Calculate, Open, Read, Write, Seek, Size, Flush, Publish,
                       Append, Sort, Consume, Remove };

enum class Coordinate { X, Y, Z };
struct SourceLocation {
    std::uint64_t line = 1;
    std::uint32_t column = 1; // one-based byte column, excluding the initial BOM
    std::optional<Coordinate> coordinate;
    QString token; // at most 64 source bytes, escaped for diagnostics
};

struct Error {
    ErrorCode code;
    Phase phase;
    Operation operation;
    std::optional<std::uint64_t> offset;
    int deviceCode = 0; // QFileDevice::FileError when available; not a native OS error code
    QString detail; // bounded to 256 UTF-16 code units by makeError()
    std::optional<SourceLocation> source;
};

Error makeError(ErrorCode code, Phase phase, Operation operation, QString detail,
                std::optional<std::uint64_t> offset = std::nullopt, int deviceCode = 0);

template<class T>
class Result {
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
inline Status success() { return std::monostate{}; }

// Caller-selected working budgets, not a universal RAM policy. Zero is not unlimited.
struct ResourceLimits {
    std::uint64_t memoryBytes = 0;
    std::uint64_t temporaryDiskBytes = 0;
    std::uint32_t maxOpenFiles = 0;
};

} // namespace cadcontour::cache
