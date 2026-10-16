#pragma once

#include "binary_codec.h"

#include <limits>

namespace cadcontour::cache {

namespace format {
inline constexpr std::uint32_t version = 1;
inline constexpr std::uint32_t byteOrder = 0x01020304;
inline constexpr std::uint32_t encoding = 1; // uncompressed RecordId + binary64 X/Y, millimetres
inline constexpr std::uint32_t checksumAlgorithm = 1; // SHA-256
inline constexpr std::size_t headerSize = 4096;
inline constexpr std::size_t subblockSize = 2048;
inline constexpr std::size_t pageSize = 65536;
inline constexpr std::size_t footerSize = 4096;
inline constexpr std::size_t checksumSize = 32;
inline constexpr std::size_t recordSize = 24;
inline constexpr std::size_t indexEntrySize = 64;
inline constexpr std::size_t pageHeaderSize = 64;
inline constexpr std::size_t pointCapacity = (pageSize - pageHeaderSize - checksumSize) / recordSize;
inline constexpr std::size_t indexCapacity = (pageSize - pageHeaderSize - checksumSize) / indexEntrySize;
inline constexpr std::uint64_t noPage = std::numeric_limits<std::uint64_t>::max();
} // namespace format

struct CacheMetadata {
    CacheId id;
    std::uint64_t pointCount = 0;
    BoundingBox bounds;
    bool hasNonZeroZ = false;
    // This type describes the implemented v1 characteristics, not arbitrary formats.
    static constexpr auto formatVersion = format::version;
    static constexpr auto coordinateEncoding = format::encoding;
};

enum class IndexKind : std::uint32_t { None = 0, BoundingBoxTree = 1 };
enum class PageKind : std::uint32_t { Points = 1, Index = 2 };

struct IndexDescriptor {
    IndexKind kind = IndexKind::None;
    std::uint64_t pageCount = 0;
    std::uint64_t rootPage = format::noPage; // relative to index section
    std::uint32_t height = 0; // root level; leaves have level 1
};

struct FileLayout {
    std::uint64_t pointPages = 0;
    std::uint64_t indexOffset = format::headerSize;
    std::uint64_t footerOffset = format::headerSize;
    std::uint64_t fileSize = format::headerSize + format::footerSize;
};

struct CacheHeader { CacheMetadata metadata; IndexDescriptor index; };
struct CompletionFooter {
    CacheId id;
    std::uint64_t pointCount = 0;
    std::uint64_t fileSize = 0;
    std::uint64_t pointPages = 0;
    std::uint64_t indexPages = 0;
    std::uint64_t footerOffset = 0;
};
struct PageHeader {
    PageKind kind = PageKind::Points;
    std::uint32_t count = 0;
    std::uint64_t pageId = 0; // ordinal relative to corresponding section
    std::uint32_t level = 0; // points: 0; index: >= 1
};
struct IndexEntry {
    BoundingBox bounds;
    std::uint64_t childPage = 0; // ordinal in section selected by childKind
    std::uint64_t pointCount = 0; // number of source records in subtree
    PageKind childKind = PageKind::Points;
};

Result<FileLayout> calculateLayout(std::uint64_t pointCount, std::uint64_t indexPages);
Status validateMetadata(const CacheMetadata &metadata);
Status encodeRecord(MutableBytes bytes, const PointRecord &record);
Result<PointRecord> decodeRecord(Bytes bytes);
Status encodeIndexEntry(MutableBytes bytes, const IndexEntry &entry);
Result<IndexEntry> decodeIndexEntry(Bytes bytes);

// Digest occupies the last 32 bytes; only preceding bytes are hashed.
Status sealChecksum(MutableBytes block);
Status verifyChecksum(Bytes block);

Status encodeHeader(MutableBytes bytes, const CacheHeader &header);
Result<CacheHeader> decodeHeader(Bytes bytes);
Status encodeFooter(MutableBytes bytes, const CompletionFooter &footer);
Result<CompletionFooter> decodeFooter(Bytes bytes);
Status validateEnvelope(const CacheHeader &header, const CompletionFooter &footer,
                        std::uint64_t actualFileSize);

// Caller owns one fixed-size buffer. No whole-file reading or page stream packing.
Status encodePointPage(MutableBytes bytes, std::uint64_t pageId, std::span<const PointRecord> records);
Status encodeIndexPage(MutableBytes bytes, std::uint64_t pageId, std::uint32_t level,
                       std::span<const IndexEntry> entries);
// Validates framing, digest, reserved/padding and entry semantics of this page only.
Result<PageHeader> validatePage(Bytes bytes);

} // namespace cadcontour::cache
