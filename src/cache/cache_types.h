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
    PublishFailed, ResourceExhausted
};
enum class Phase { Codec, Layout, FileIO, Publication };
enum class Operation { Encode, Decode, Calculate, Open, Read, Write, Seek, Size, Flush, Publish };

struct Error {
    ErrorCode code;
    Phase phase;
    Operation operation;
    std::optional<std::uint64_t> offset;
    int deviceCode = 0; // QFileDevice::FileError when available; not a native OS error code
    QString detail; // bounded to 256 UTF-16 code units by makeError()
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

// Caller-selected limits, not a universal RAM policy. Future ingest validates its
// concrete buffer/fan-in requirements against these budgets before allocating.
struct ResourceLimits {
    std::uint64_t memoryBytes = 0;
    std::uint64_t temporaryDiskBytes = 0;
    std::uint32_t maxOpenFiles = 0;
};

} // namespace cadcontour::cache
