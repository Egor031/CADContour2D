#include "point_cache_format.h"

#include <QCryptographicHash>

#include <algorithm>
#include <bit>
#include <cstring>

namespace cadcontour::cache {
namespace {
using namespace format;
using namespace binary;

constexpr char dataMagic[] = "PC2DDATA";
constexpr char indexMagic[] = "PC2DINDX";
constexpr char pageMagic[] = "PC2DPAGE";
constexpr char footerMagic[] = "PC2DDONE";

Error invalid(QString detail, Operation op = Operation::Decode)
{
    return makeError(op == Operation::Encode ? ErrorCode::InvalidInput : ErrorCode::CorruptCache,
                     Phase::Codec, op, std::move(detail));
}
Error unsupported(QString detail)
{
    return makeError(ErrorCode::UnsupportedFormat, Phase::Codec, Operation::Decode, std::move(detail));
}
bool zero(Bytes bytes)
{
    return std::all_of(bytes.begin(), bytes.end(), [](auto b) { return b == std::byte{0}; });
}
bool magic(Bytes b, const char *value) { return std::memcmp(b.data(), value, 8) == 0; }
void putId(MutableBytes b, std::size_t o, const CacheId &id)
{
    std::copy(id.bytes.begin(), id.bytes.end(), b.begin() + o);
}
CacheId getId(Bytes b, std::size_t o)
{
    CacheId id;
    std::copy_n(b.begin() + o, id.bytes.size(), id.bytes.begin());
    return id;
}
void putBounds(MutableBytes b, std::size_t o, BoundingBox v)
{
    putDouble(b, o, v.minX); putDouble(b, o + 8, v.minY);
    putDouble(b, o + 16, v.maxX); putDouble(b, o + 24, v.maxY);
}
BoundingBox getBounds(Bytes b, std::size_t o)
{
    return {*getDouble(b, o), *getDouble(b, o + 8), *getDouble(b, o + 16), *getDouble(b, o + 24)};
}
Status checkHeader(const CacheHeader &h)
{
    if (auto s = validateMetadata(h.metadata); !s) return s;
    const auto &i = h.index;
    if (h.metadata.pointCount == 0) {
        if (i.kind != IndexKind::None || i.pageCount != 0 || i.rootPage != noPage || i.height != 0)
            return invalid(QStringLiteral("Empty dataset must have no index"), Operation::Encode);
    } else if (i.kind != IndexKind::BoundingBoxTree || i.pageCount == 0
               || i.rootPage >= i.pageCount || i.height == 0 || i.height > i.pageCount) {
        return invalid(QStringLiteral("Invalid index descriptor"), Operation::Encode);
    }
    if (auto layout = calculateLayout(h.metadata.pointCount, i.pageCount); !layout)
        return layout.error();
    return success();
}
Status checkFooter(const CompletionFooter &f)
{
    const auto layout = calculateLayout(f.pointCount, f.indexPages);
    if (!layout || f.id.isNull() || f.pointPages != layout.value().pointPages
        || f.fileSize != layout.value().fileSize || f.footerOffset != layout.value().footerOffset
        || (f.pointCount == 0) != (f.indexPages == 0))
        return invalid(QStringLiteral("Invalid completion layout"), Operation::Encode);
    return success();
}
Error decodedError(const Error &error)
{
    return invalid(error.detail);
}
bool validEntry(const IndexEntry &e)
{
    return validBounds(e.bounds) && e.childPage != noPage && e.pointCount != 0
        && (e.childKind == PageKind::Points || e.childKind == PageKind::Index);
}
void initPage(MutableBytes b, PageHeader h, std::size_t entrySize)
{
    std::fill(b.begin(), b.end(), std::byte{0});
    std::memcpy(b.data(), pageMagic, 8);
    putU32(b, 8, version); putU32(b, 12, static_cast<std::uint32_t>(h.kind));
    putU32(b, 20, h.count); putU64(b, 24, h.pageId); putU32(b, 32, h.level);
    putU32(b, 36, static_cast<std::uint32_t>(h.count * entrySize));
}
} // namespace

Result<FileLayout> calculateLayout(std::uint64_t pointCount, std::uint64_t indexPages)
{
    const auto points = *checked::pageCount(pointCount, format::pointCapacity);
    const auto dataBytes = checked::multiply(points, format::pageSize);
    const auto indexBytes = checked::multiply(indexPages, format::pageSize);
    const auto indexOffset = dataBytes ? checked::add(format::headerSize, *dataBytes) : std::nullopt;
    const auto footerOffset = indexOffset && indexBytes ? checked::add(*indexOffset, *indexBytes) : std::nullopt;
    const auto fileSize = footerOffset ? checked::add(*footerOffset, format::footerSize) : std::nullopt;
    if (!fileSize || !checked::fileOffset(*fileSize))
        return makeError(ErrorCode::InvalidInput, Phase::Layout, Operation::Calculate,
                         QStringLiteral("File layout exceeds representable offsets"));
    return FileLayout{points, *indexOffset, *footerOffset, *fileSize};
}

Status validateMetadata(const CacheMetadata &m)
{
    if (m.id.isNull() || !validBounds(m.bounds))
        return invalid(QStringLiteral("Invalid identity or bounds"), Operation::Encode);
    if (m.pointCount == 0 && (m.hasNonZeroZ
        || std::bit_cast<std::uint64_t>(m.bounds.minX) != 0
        || std::bit_cast<std::uint64_t>(m.bounds.minY) != 0
        || std::bit_cast<std::uint64_t>(m.bounds.maxX) != 0
        || std::bit_cast<std::uint64_t>(m.bounds.maxY) != 0))
        return invalid(QStringLiteral("Empty bounds must be positive zero and Z flag clear"), Operation::Encode);
    return success();
}

Status encodeRecord(MutableBytes b, const PointRecord &r)
{
    if (b.size() != format::recordSize || !validPoint(r.point))
        return invalid(QStringLiteral("Invalid point record or buffer size"), Operation::Encode);
    putU64(b, 0, r.id); putDouble(b, 8, r.point.x); putDouble(b, 16, r.point.y);
    return success();
}
Result<PointRecord> decodeRecord(Bytes b)
{
    if (b.size() != format::recordSize) return invalid(QStringLiteral("Invalid record size"));
    PointRecord r{*getU64(b, 0), {*getDouble(b, 8), *getDouble(b, 16)}};
    if (!validPoint(r.point)) return invalid(QStringLiteral("Non-finite dataset coordinate"));
    return r;
}
Status encodeIndexEntry(MutableBytes b, const IndexEntry &e)
{
    if (b.size() != format::indexEntrySize || !validEntry(e))
        return invalid(QStringLiteral("Invalid index entry or buffer size"), Operation::Encode);
    std::fill(b.begin(), b.end(), std::byte{0});
    putBounds(b, 0, e.bounds); putU64(b, 32, e.childPage); putU64(b, 40, e.pointCount);
    putU32(b, 48, static_cast<std::uint32_t>(e.childKind));
    return success();
}
Result<IndexEntry> decodeIndexEntry(Bytes b)
{
    if (b.size() != format::indexEntrySize) return invalid(QStringLiteral("Invalid index entry size"));
    if (*getU32(b, 48) != 1 && *getU32(b, 48) != 2)
        return unsupported(QStringLiteral("Unknown child page kind"));
    IndexEntry e{getBounds(b, 0), *getU64(b, 32), *getU64(b, 40), static_cast<PageKind>(*getU32(b, 48))};
    if (!validEntry(e) || !zero(b.subspan(52))) return invalid(QStringLiteral("Invalid index entry fields"));
    return e;
}

Status sealChecksum(MutableBytes b)
{
    if (b.size() < format::checksumSize || b.size() > static_cast<std::size_t>(std::numeric_limits<qsizetype>::max()))
        return invalid(QStringLiteral("Invalid checksum buffer size"), Operation::Encode);
    const auto size = b.size() - format::checksumSize;
    const auto digest = QCryptographicHash::hashInto(QSpan<std::byte>(b.data() + size, format::checksumSize),
        QByteArrayView(reinterpret_cast<const char *>(b.data()), static_cast<qsizetype>(size)), QCryptographicHash::Sha256);
    if (digest.size() != format::checksumSize)
        return makeError(ErrorCode::ResourceExhausted, Phase::Codec, Operation::Encode, QStringLiteral("SHA-256 unavailable"));
    return success();
}
Status verifyChecksum(Bytes b)
{
    if (b.size() < format::checksumSize || b.size() > static_cast<std::size_t>(std::numeric_limits<qsizetype>::max()))
        return invalid(QStringLiteral("Invalid checksum buffer size"));
    std::array<std::byte, format::checksumSize> digest{};
    const auto size = b.size() - digest.size();
    const auto result = QCryptographicHash::hashInto(QSpan<std::byte>(digest.data(), digest.size()),
        QByteArrayView(reinterpret_cast<const char *>(b.data()), static_cast<qsizetype>(size)), QCryptographicHash::Sha256);
    if (result.size() != format::checksumSize)
        return makeError(ErrorCode::ResourceExhausted, Phase::Codec, Operation::Decode, QStringLiteral("SHA-256 unavailable"));
    if (!std::equal(digest.begin(), digest.end(), b.begin() + size))
        return invalid(QStringLiteral("SHA-256 mismatch"));
    return success();
}

Status encodeHeader(MutableBytes b, const CacheHeader &h)
{
    if (b.size() != headerSize) return invalid(QStringLiteral("Invalid header size"), Operation::Encode);
    if (auto s = checkHeader(h); !s) return s;
    const auto layout = calculateLayout(h.metadata.pointCount, h.index.pageCount).value();
    std::fill(b.begin(), b.end(), std::byte{0});
    auto d = b.first(subblockSize);
    std::memcpy(d.data(), dataMagic, 8);
    putU32(d, 8, version); putU32(d, 12, byteOrder); putU32(d, 16, headerSize);
    putU32(d, 20, pageSize); putU32(d, 24, recordSize); putU32(d, 28, encoding);
    putU32(d, 32, checksumAlgorithm); putU32(d, 36, h.metadata.hasNonZeroZ ? 1 : 0);
    putId(d, 40, h.metadata.id); putU64(d, 56, h.metadata.pointCount); putBounds(d, 64, h.metadata.bounds);
    putU64(d, 96, headerSize); putU64(d, 104, layout.pointPages);
    if (auto s = sealChecksum(d); !s) return s;
    auto i = b.subspan(subblockSize);
    std::memcpy(i.data(), indexMagic, 8);
    putU32(i, 8, version); putU32(i, 16, pageSize); putU32(i, 20, indexEntrySize);
    putU32(i, 24, checksumAlgorithm); putU32(i, 28, static_cast<std::uint32_t>(h.index.kind));
    putU64(i, 32, layout.indexOffset); putU64(i, 40, h.index.pageCount);
    putU64(i, 48, h.index.rootPage); putU32(i, 56, h.index.height);
    return sealChecksum(i);
}

Result<CacheHeader> decodeHeader(Bytes b)
{
    if (b.size() != headerSize)
        return makeError(ErrorCode::IncompleteCache, Phase::Codec, Operation::Decode, QStringLiteral("Header size mismatch"));
    auto d = b.first(subblockSize); auto i = b.subspan(subblockSize);
    if (auto s = verifyChecksum(d); !s) return s.error();
    if (auto s = verifyChecksum(i); !s) return s.error();
    if (!magic(d, dataMagic) || !magic(i, indexMagic)) return invalid(QStringLiteral("Header magic mismatch"));
    if (*getU32(d, 8) != version || *getU32(i, 8) != version || *getU32(d, 12) != byteOrder
        || *getU32(d, 28) != encoding || *getU32(d, 32) != checksumAlgorithm || *getU32(i, 24) != checksumAlgorithm
        || (*getU32(d, 36) & ~1u) != 0 || *getU32(i, 12) != 0 || *getU32(i, 28) > 1)
        return unsupported(QStringLiteral("Unsupported header version, encoding or flags"));
    if (*getU32(d, 16) != headerSize || *getU32(d, 20) != pageSize || *getU32(d, 24) != recordSize
        || *getU32(i, 16) != pageSize || *getU32(i, 20) != indexEntrySize
        || *getU64(d, 96) != headerSize || !zero(d.subspan(112, subblockSize - checksumSize - 112))
        || !zero(i.subspan(60, subblockSize - checksumSize - 60)))
        return invalid(QStringLiteral("Invalid header sizes, offsets or reserved bytes"));
    CacheHeader h{{getId(d, 40), *getU64(d, 56), getBounds(d, 64), *getU32(d, 36) != 0},
                 {static_cast<IndexKind>(*getU32(i, 28)), *getU64(i, 40), *getU64(i, 48), *getU32(i, 56)}};
    if (auto s = checkHeader(h); !s) return decodedError(s.error());
    const auto layout = calculateLayout(h.metadata.pointCount, h.index.pageCount).value();
    if (*getU64(d, 104) != layout.pointPages || *getU64(i, 32) != layout.indexOffset)
        return invalid(QStringLiteral("Inconsistent header layout"));
    return h;
}

Status encodeFooter(MutableBytes b, const CompletionFooter &f)
{
    if (b.size() != footerSize) return invalid(QStringLiteral("Invalid footer size"), Operation::Encode);
    if (auto s = checkFooter(f); !s) return s;
    std::fill(b.begin(), b.end(), std::byte{0});
    std::memcpy(b.data(), footerMagic, 8); putU32(b, 8, version); putU32(b, 12, 1);
    putId(b, 16, f.id); putU64(b, 32, f.pointCount); putU64(b, 40, f.fileSize);
    putU64(b, 48, f.pointPages); putU64(b, 56, f.indexPages); putU64(b, 64, f.footerOffset);
    return sealChecksum(b);
}
Result<CompletionFooter> decodeFooter(Bytes b)
{
    if (b.size() != footerSize)
        return makeError(ErrorCode::IncompleteCache, Phase::Codec, Operation::Decode, QStringLiteral("Footer size mismatch"));
    if (auto s = verifyChecksum(b); !s) return s.error();
    if (!magic(b, footerMagic)) return invalid(QStringLiteral("Footer magic mismatch"));
    if (*getU32(b, 8) != version) return unsupported(QStringLiteral("Unsupported footer version"));
    if (*getU32(b, 12) != 1)
        return makeError(ErrorCode::IncompleteCache, Phase::Codec, Operation::Decode, QStringLiteral("Missing completion marker"));
    if (!zero(b.subspan(72, footerSize - checksumSize - 72))) return invalid(QStringLiteral("Nonzero footer reserved bytes"));
    CompletionFooter f{getId(b, 16), *getU64(b, 32), *getU64(b, 40), *getU64(b, 48), *getU64(b, 56), *getU64(b, 64)};
    if (auto s = checkFooter(f); !s) return decodedError(s.error());
    return f;
}
Status validateEnvelope(const CacheHeader &h, const CompletionFooter &f, std::uint64_t actual)
{
    if (auto s = checkHeader(h); !s) return decodedError(s.error());
    if (auto s = checkFooter(f); !s) return decodedError(s.error());
    const auto layout = calculateLayout(h.metadata.pointCount, h.index.pageCount).value();
    if (actual < layout.fileSize)
        return makeError(ErrorCode::IncompleteCache, Phase::Codec, Operation::Decode, QStringLiteral("Truncated container"));
    if (h.metadata.id != f.id || h.metadata.pointCount != f.pointCount || h.index.pageCount != f.indexPages
        || f.fileSize != layout.fileSize || actual != layout.fileSize)
        return invalid(QStringLiteral("Header/footer/file size mismatch"));
    return success();
}

Status encodePointPage(MutableBytes b, std::uint64_t id, std::span<const PointRecord> records)
{
    if (b.size() != pageSize || records.empty() || records.size() > pointCapacity || id == noPage)
        return invalid(QStringLiteral("Invalid point page dimensions"), Operation::Encode);
    for (const auto &r : records)
        if (!validPoint(r.point)) return invalid(QStringLiteral("Non-finite point"), Operation::Encode);
    initPage(b, {PageKind::Points, static_cast<std::uint32_t>(records.size()), id, 0}, recordSize);
    for (std::size_t n = 0; n < records.size(); ++n)
        encodeRecord(b.subspan(pageHeaderSize + n * recordSize, recordSize), records[n]);
    return sealChecksum(b);
}
Status encodeIndexPage(MutableBytes b, std::uint64_t id, std::uint32_t level, std::span<const IndexEntry> entries)
{
    if (b.size() != pageSize || entries.empty() || entries.size() > indexCapacity || id == noPage || level == 0)
        return invalid(QStringLiteral("Invalid index page dimensions"), Operation::Encode);
    for (const auto &e : entries)
        if (!validEntry(e) || e.childKind != (level == 1 ? PageKind::Points : PageKind::Index))
            return invalid(QStringLiteral("Index child kind conflicts with level"), Operation::Encode);
    initPage(b, {PageKind::Index, static_cast<std::uint32_t>(entries.size()), id, level}, indexEntrySize);
    for (std::size_t n = 0; n < entries.size(); ++n)
        encodeIndexEntry(b.subspan(pageHeaderSize + n * indexEntrySize, indexEntrySize), entries[n]);
    return sealChecksum(b);
}
Result<PageHeader> validatePage(Bytes b)
{
    if (b.size() != pageSize)
        return makeError(ErrorCode::IncompleteCache, Phase::Codec, Operation::Decode, QStringLiteral("Page size mismatch"));
    if (auto s = verifyChecksum(b); !s) return s.error();
    if (!magic(b, pageMagic)) return invalid(QStringLiteral("Page magic mismatch"));
    const auto kind = *getU32(b, 12);
    if (*getU32(b, 8) != version || *getU32(b, 16) != 0 || (kind != 1 && kind != 2))
        return unsupported(QStringLiteral("Unsupported page version, flags or kind"));
    const auto count = *getU32(b, 20); const auto level = *getU32(b, 32);
    const auto capacity = kind == 1 ? pointCapacity : indexCapacity;
    const auto entrySize = kind == 1 ? recordSize : indexEntrySize;
    if (count == 0 || count > capacity || *getU64(b, 24) == noPage || (kind == 1 ? level != 0 : level == 0)
        || *getU32(b, 36) != count * entrySize || !zero(b.subspan(40, 24)))
        return invalid(QStringLiteral("Invalid page fields"));
    const auto used = pageHeaderSize + count * entrySize;
    if (!zero(b.subspan(used, pageSize - checksumSize - used))) return invalid(QStringLiteral("Nonzero page padding"));
    for (std::size_t n = 0; n < count; ++n) {
        const auto bytes = b.subspan(pageHeaderSize + n * entrySize, entrySize);
        if (kind == 1) {
            if (auto r = decodeRecord(bytes); !r) return r.error();
        } else {
            const auto e = decodeIndexEntry(bytes);
            if (!e) return e.error();
            if (e.value().childKind != (level == 1 ? PageKind::Points : PageKind::Index))
                return invalid(QStringLiteral("Invalid index level/child relationship"));
        }
    }
    return PageHeader{static_cast<PageKind>(kind), count, *getU64(b, 24), level};
}

} // namespace cadcontour::cache
