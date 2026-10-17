#pragma once

#include "cache/cache_types.h"

#include <QIODevice>

#include <functional>
#include <span>

namespace cadcontour::io {

inline constexpr std::size_t ascReadBytes = 65536;
inline constexpr std::size_t ascLineBytes = 65536;
inline constexpr std::size_t ascBatchSize = 4096;
// Fixed arrays plus control/stack/Qt allowance; excludes caller-owned source/sink state.
inline constexpr std::uint64_t ascParserReserve = ascReadBytes + ascLineBytes
    + ascBatchSize * sizeof(cache::Measurement) + 65536;

using CancelCheck = bool (*)(void *) noexcept;
enum class ZDecision { Continue, Cancel };
struct NonZeroZ { std::uint64_t line; double value; };
using ZHandler = std::function<ZDecision(NonZeroZ)>;
using MeasurementSink = std::function<cache::Status(std::span<const cache::Measurement>)>;
struct SourceSummary {
    std::uint64_t bytesRead = 0;
    std::uint64_t physicalLines = 0;
    std::uint64_t measurements = 0;
    std::optional<NonZeroZ> firstNonZeroZ;
};

// Synchronous, one pass, borrowed binary source at its beginning. Short reads are legal;
// zero progress requires confirmed EOF. Source/sink/handlers must not reenter or mutate
// the parser. A sink may see a prefix before failure; it must stage its result.
// Nonzero Z requires a handler; its single request precedes further reads/sink calls.
cache::Result<SourceSummary> parseAscXyz(
    QIODevice &source, const MeasurementSink &sink, const ZHandler &zHandler,
    CancelCheck cancel = nullptr, void *context = nullptr);

} // namespace cadcontour::io
