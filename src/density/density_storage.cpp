#include "density_storage.h"

#include <QElapsedTimer>
#include <QFileInfo>
#include <QDir>
#include <zlib.h>

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>

namespace cadcontour::density {
namespace {
using cache::Bytes;
using cache::MutableBytes;
using namespace cache::binary;
constexpr U64 headerSize = 320, entrySize = 80, footerSize = 64, ioChunk = 65536;
constexpr U64 maximumRun = 4096;
constexpr U64 maximum = std::numeric_limits<U64>::max();
struct Failure { Error error; };
Error error(ErrorCode code, Phase phase, const char *detail)
{
    return {code, phase, QString::fromLatin1(detail).left(256)};
}
[[noreturn]] void fail(ErrorCode code, Phase phase, const char *detail) { throw Failure{error(code, phase, detail)}; }
void need(bool condition, ErrorCode code, Phase phase, const char *detail)
{
    if (!condition) fail(code, phase, detail);
}
U64 add(U64 a, U64 b, ErrorCode code = ErrorCode::ResourceRefused, Phase phase = Phase::Metadata)
{
    auto n = cache::checked::add(a, b);
    if (!n) fail(code, phase, "uint64 addition overflow");
    return *n;
}
U64 multiply(U64 a, U64 b, ErrorCode code = ErrorCode::ResourceRefused, Phase phase = Phase::Metadata)
{
    auto n = cache::checked::multiply(a, b);
    if (!n) fail(code, phase, "uint64 multiplication overflow");
    return *n;
}
void budget(U64 required, U64 allowed, Phase phase, const char *detail)
{
    if (required > allowed) {
        auto e = error(ErrorCode::ResourceRefused, phase, detail);
        e.required = required; e.budget = allowed;
        throw Failure{std::move(e)};
    }
}
std::size_t memorySize(U64 n)
{
    auto s = cache::checked::memorySize(n);
    if (!s || n > static_cast<U64>(std::numeric_limits<std::ptrdiff_t>::max()))
        fail(ErrorCode::ResourceRefused, Phase::Metadata, "Unrepresentable buffer size");
    return *s;
}
void cacheStatus(const cache::Status &status, Phase phase)
{
    if (status) return;
    const auto &source = status.error();
    auto e = error(source.code == cache::ErrorCode::IncompleteCache ? ErrorCode::IncompleteDataset
        : source.code == cache::ErrorCode::ResourceExhausted ? ErrorCode::ResourceRefused : ErrorCode::IO,
        phase, "File operation failed");
    e.detail = source.detail.left(256); e.offset = source.offset; e.deviceCode = source.deviceCode;
    throw Failure{std::move(e)};
}
[[noreturn]] void deviceFailure(QFileDevice &device, Phase phase)
{
    auto e = error(ErrorCode::IO, phase, "Device operation failed");
    e.deviceCode = static_cast<int>(device.error()); e.detail = device.errorString().left(256);
    if (device.pos() >= 0) e.offset = static_cast<U64>(device.pos());
    throw Failure{std::move(e)};
}
template<class T, class F> Result<T> attempt(Phase phase, F &&function)
{
    try { return function(); }
    catch (const Failure &f) { return f.error; }
    catch (const std::bad_alloc &) { return Error{ErrorCode::ResourceRefused, phase, QStringLiteral("Allocation failed")}; }
    catch (const std::length_error &) { return Error{ErrorCode::ResourceRefused, phase, QStringLiteral("Container size unrepresentable")}; }
}
std::uint32_t crc(Bytes bytes, std::uint32_t initial = 0)
{
    return static_cast<std::uint32_t>(crc32_z(initial, reinterpret_cast<const Bytef *>(bytes.data()), bytes.size()));
}
bool less(TileKey a, TileKey b) { return a.y < b.y || (a.y == b.y && a.x < b.x); }
Shape shape(U64 width, U64 height, std::uint32_t side, TileKey key)
{
    need(width && height && (side == 256 || side == 512), ErrorCode::InvalidInput, Phase::Metadata, "Invalid dimensions/tile side");
    // Division first proves both subsequent products and subtractions safe.
    need(key.x <= (width - 1) / side && key.y <= (height - 1) / side,
         ErrorCode::InvalidInput, Phase::Read, "Tile key outside logical bounds");
    return {static_cast<std::uint32_t>(std::min<U64>(side, width - key.x * side)),
            static_cast<std::uint32_t>(std::min<U64>(side, height - key.y * side))};
}
U64 bufferReserve(std::uint32_t side)
{
    need(side == 256 || side == 512, ErrorCode::UnsupportedFormat, Phase::Metadata, "Unsupported tile side");
    const U64 raw = multiply(multiply(side, side), 8);
    need(raw <= std::numeric_limits<uLong>::max(), ErrorCode::ResourceRefused, Phase::Metadata, "zlib size unrepresentable");
    return add(add(multiply(raw, 2), compressBound(static_cast<uLong>(raw))), ioChunk);
}
struct Context {
    ResourcePolicy policy;
    Statistics stats;
    Phase phase = Phase::Metadata;
    CancelCheck cancel = nullptr;
    void *cancelContext = nullptr;
    Status (*hook)(Phase, void *) noexcept = nullptr;
    void *hookContext = nullptr;
    void check()
    {
        if (cancel && cancel(cancelContext)) fail(ErrorCode::Cancelled, phase, "Cancelled");
        budget(add(stats.workUnits, 1), policy.workUnits, phase, "Work budget exhausted");
        ++stats.workUnits;
        if (hook) { auto s = hook(phase, hookContext); if (!s) throw Failure{s.error()}; }
    }
    void read(QIODevice &device, U64 offset, MutableBytes bytes)
    {
        need(cache::checked::range(offset, bytes.size(), static_cast<U64>(std::numeric_limits<qint64>::max())),
             ErrorCode::CorruptData, phase, "Read range unrepresentable");
        check(); cacheStatus(cache::file::seek(device, offset), phase);
        for (std::size_t done = 0; done < bytes.size();) {
            check(); auto n = std::min<std::size_t>(bytes.size() - done, ioChunk);
            cacheStatus(cache::file::readExact(device, bytes.subspan(done, n)), phase);
            stats.readBytes = add(stats.readBytes, n); done += n;
        }
    }
    void write(QIODevice &device, U64 offset, Bytes bytes)
    {
        need(cache::checked::range(offset, bytes.size(), static_cast<U64>(std::numeric_limits<qint64>::max())),
             ErrorCode::ResourceRefused, phase, "Write range unrepresentable");
        check(); cacheStatus(cache::file::seek(device, offset), phase);
        for (std::size_t done = 0; done < bytes.size();) {
            check(); auto n = std::min<std::size_t>(bytes.size() - done, ioChunk);
            cacheStatus(cache::file::writeExact(device, bytes.subspan(done, n)), phase);
            stats.writtenBytes = add(stats.writtenBytes, n); done += n;
        }
    }
};
raster::Mapping mapping(ModelPoint minimum, ModelPoint maximumPoint, double cell, Context &c)
{
    QElapsedTimer timer; timer.start();
    // Forward the original noexcept callback. Mapping rechecks FP environment after it.
    const auto result = raster::Mapping::create(minimum, maximumPoint, cell, c.policy.mapping, c.cancel, c.cancelContext);
    c.stats.mappingNanoseconds = add(c.stats.mappingNanoseconds, static_cast<U64>(timer.nsecsElapsed()));
    if (result) return result.value();
    const auto &source = result.error();
    auto code = source.category() == raster::ErrorCategory::Cancelled ? ErrorCode::Cancelled
        : source.category() == raster::ErrorCategory::ResourceRefused ? ErrorCode::ResourceRefused : ErrorCode::Mapping;
    auto e = error(code, Phase::Metadata, "Raster mapping validation failed");
    e.mapping = source; e.required = source.requiredBoundaryChecks; e.budget = c.policy.mapping.maxBoundaryChecks;
    throw Failure{std::move(e)};
}
struct Entry {
    TileKey key;
    U64 offset = 0, compressed = 0, raw = 0, total = 0, maximum = 0;
    std::uint32_t checksum = 0, encoding = 0;
    Shape shape{};
};
std::array<std::byte, entrySize> encode(const Entry &e)
{
    std::array<std::byte, entrySize> b{};
    putU64(b, 0, e.key.x); putU64(b, 8, e.key.y); putU64(b, 16, e.offset);
    putU64(b, 24, e.compressed); putU64(b, 32, e.raw); putU64(b, 40, e.total); putU64(b, 48, e.maximum);
    putU32(b, 56, e.checksum); putU32(b, 60, e.encoding);
    putU32(b, 64, e.shape.width); putU32(b, 68, e.shape.height);
    return b;
}
Entry decode(Bytes b)
{
    need(*getU64(b, 72) == 0, ErrorCode::CorruptData, Phase::Index, "Nonzero index reserved bytes");
    return {{*getU64(b, 0), *getU64(b, 8)}, *getU64(b, 16), *getU64(b, 24), *getU64(b, 32),
            *getU64(b, 40), *getU64(b, 48), *getU32(b, 56), *getU32(b, 60), {*getU32(b, 64), *getU32(b, 68)}};
}
struct Header {
    Metadata metadata;
    U64 tiles = 0, total = 0, payload = 0, indexOffset = headerSize, indexLength = 0, footerOffset = headerSize;
    U64 fileSize = headerSize + footerSize;
    std::uint32_t indexCrc = 0, headerCrc = 0;
};
std::array<std::byte, headerSize> encodeHeader(const Header &h)
{
    std::array<std::byte, headerSize> b{};
    std::memcpy(b.data(), "PC2DDN01", 8);
    putU32(b, 8, 1); putU32(b, 12, headerSize); putU32(b, 20, 1); putU32(b, 24, 1);
    putU32(b, 28, h.metadata.tileSide); putU32(b, 32, 1); putU32(b, 36, 6); putU32(b, 40, 3);
    putU32(b, 44, h.metadata.contract == BuildContract::SuppliedTiles ? 1 : 2);
    std::copy(h.metadata.id.bytes.begin(), h.metadata.id.bytes.end(), b.begin() + 48);
    std::copy(h.metadata.source.bytes.begin(), h.metadata.source.bytes.end(), b.begin() + 64);
    putU64(b, 80, h.metadata.sourceMeasurements);
    const auto &m = h.metadata.mapping;
    putDouble(b, 88, m.minimum().x); putDouble(b, 96, m.minimum().y);
    putDouble(b, 104, m.maximum().x); putDouble(b, 112, m.maximum().y); putDouble(b, 120, m.cell());
    putDouble(b, 128, m.origin().x); putDouble(b, 136, m.origin().y);
    putU64(b, 144, m.width()); putU64(b, 152, m.height()); putU64(b, 160, h.tiles); putU64(b, 168, h.total);
    putU64(b, 176, headerSize); putU64(b, 184, h.payload); putU64(b, 192, h.indexOffset);
    putU64(b, 200, h.indexLength); putU64(b, 208, h.footerOffset); putU64(b, 216, footerSize); putU64(b, 224, h.fileSize);
    putU32(b, 232, h.indexCrc); putU32(b, 240, 1); putU32(b, 236, crc(b));
    return b;
}
std::array<std::byte, footerSize> encodeFooter(const Header &h)
{
    std::array<std::byte, footerSize> b{};
    std::memcpy(b.data(), "PC2DEND1", 8); putU32(b, 8, 1); putU64(b, 16, h.fileSize);
    putU64(b, 24, h.tiles); putU64(b, 32, h.total); putU32(b, 40, h.headerCrc); putU32(b, 44, h.indexCrc);
    putU32(b, 48, crc(b)); return b;
}
Header decodeHeader(std::array<std::byte, headerSize> b, Context &c)
{
    need(std::memcmp(b.data(), "PC2DDN01", 8) == 0 && *getU32(b, 8) == 1 && *getU32(b, 12) == headerSize,
         ErrorCode::UnsupportedFormat, Phase::Open, "Unsupported density magic/version/header size");
    const auto checksum = *getU32(b, 236); putU32(b, 236, 0);
    need(crc(b) == checksum, ErrorCode::CorruptData, Phase::Open, "Header checksum mismatch");
    need(*getU32(b, 16) == 0 && *getU32(b, 20) == 1 && *getU32(b, 24) == 1
        && (*getU32(b, 28) == 256 || *getU32(b, 28) == 512) && *getU32(b, 32) == 1
        && *getU32(b, 36) == 6 && *getU32(b, 40) == 3
        && (*getU32(b, 44) == 1 || *getU32(b, 44) == 2) && *getU32(b, 240) == 1,
         ErrorCode::UnsupportedFormat, Phase::Open, "Unsupported density flags/units/convention/codec/build contract");
    need(std::all_of(b.begin() + 244, b.end(), [](auto v) { return v == std::byte{}; }),
         ErrorCode::CorruptData, Phase::Open, "Nonzero header reserved bytes");
    DatasetId id; cache::CacheId source;
    std::copy_n(b.begin() + 48, 16, id.bytes.begin()); std::copy_n(b.begin() + 64, 16, source.bytes.begin());
    need(!id.isNull() && !source.isNull(), ErrorCode::CorruptData, Phase::Open, "Null dataset/source UUID");
    need(*getU64(b, 88) == *getU64(b, 128) && *getU64(b, 96) == *getU64(b, 136),
         ErrorCode::CorruptData, Phase::Open, "Origin/minimum bits mismatch");
    auto m = mapping({*getDouble(b, 88), *getDouble(b, 96)}, {*getDouble(b, 104), *getDouble(b, 112)}, *getDouble(b, 120), c);
    need(m.width() == *getU64(b, 144) && m.height() == *getU64(b, 152),
         ErrorCode::CorruptData, Phase::Open, "Mapping dimensions mismatch");
    Header h{{id, source, *getU64(b, 80), *getU32(b, 44) == 1 ? BuildContract::SuppliedTiles : BuildContract::CompletePointCache,
              *getU32(b, 28), m}};
    h.tiles = *getU64(b, 160); h.total = *getU64(b, 168); h.payload = *getU64(b, 184);
    h.indexOffset = *getU64(b, 192); h.indexLength = *getU64(b, 200); h.footerOffset = *getU64(b, 208);
    h.fileSize = *getU64(b, 224); h.indexCrc = *getU32(b, 232); h.headerCrc = checksum;
    need(*getU64(b, 176) == headerSize && *getU64(b, 216) == footerSize
        && h.indexOffset == add(headerSize, h.payload, ErrorCode::CorruptData, Phase::Open)
        && h.indexLength == multiply(h.tiles, entrySize, ErrorCode::CorruptData, Phase::Open)
        && h.footerOffset == add(h.indexOffset, h.indexLength, ErrorCode::CorruptData, Phase::Open)
        && h.fileSize == add(h.footerOffset, footerSize, ErrorCode::CorruptData, Phase::Open)
        && cache::checked::fileOffset(h.fileSize).has_value(), ErrorCode::CorruptData, Phase::Open, "Invalid section layout");
    need(h.metadata.contract != BuildContract::CompletePointCache || h.total == h.metadata.sourceMeasurements,
         ErrorCode::CorruptData, Phase::Open, "Complete Point Cache count contract mismatch");
    return h;
}
void validateFooter(std::array<std::byte, footerSize> b, const Header &h)
{
    need(std::memcmp(b.data(), "PC2DEND1", 8) == 0, ErrorCode::IncompleteDataset, Phase::Open, "Missing completion record");
    need(*getU32(b, 8) == 1 && *getU32(b, 12) == 0, ErrorCode::UnsupportedFormat, Phase::Open, "Unsupported completion version/flags");
    const auto checksum = *getU32(b, 48); putU32(b, 48, 0);
    need(crc(b) == checksum && *getU64(b, 16) == h.fileSize && *getU64(b, 24) == h.tiles
        && *getU64(b, 32) == h.total && *getU32(b, 40) == h.headerCrc && *getU32(b, 44) == h.indexCrc
        && std::all_of(b.begin() + 52, b.end(), [](auto v) { return v == std::byte{}; }),
         ErrorCode::CorruptData, Phase::Open, "Invalid completion/checksum");
}
// Allocation headers are max_align_t aligned; charged bytes include the header.
struct alignas(std::max_align_t) Allocation { U64 bytes; };
struct Codec {
    z_stream stream{};
    U64 used = 0, peak = 0, limit;
    bool encoding, initialized = false;
    static voidpf allocate(voidpf opaque, uInt items, uInt size) noexcept
    {
        auto &self = *static_cast<Codec *>(opaque);
        const auto bytes = cache::checked::multiply(items, size);
        const auto charged = bytes ? cache::checked::add(*bytes, sizeof(Allocation)) : std::nullopt;
        if (!charged || *charged > self.limit - self.used || !cache::checked::memorySize(*charged)) return nullptr;
        auto p = static_cast<Allocation *>(std::calloc(1, static_cast<std::size_t>(*charged)));
        if (!p) return nullptr;
        p->bytes = *charged; self.used += *charged; self.peak = std::max(self.peak, self.used); return p + 1;
    }
    static void free(voidpf opaque, voidpf address) noexcept
    {
        if (!address) return;
        auto &self = *static_cast<Codec *>(opaque); auto p = static_cast<Allocation *>(address) - 1;
        self.used -= p->bytes; std::free(p);
    }
    Codec(bool encode, U64 budgetBytes) : limit(budgetBytes), encoding(encode)
    {
        stream.zalloc = allocate; stream.zfree = free; stream.opaque = this;
    }
    ~Codec() { if (initialized) { if (encoding) deflateEnd(&stream); else inflateEnd(&stream); } }
};
std::vector<std::byte> compress(Bytes raw, Context &c)
{
    Codec codec(true, c.policy.codecBytes);
    int code = deflateInit2(&codec.stream, 6, Z_DEFLATED, 15, 8, Z_DEFAULT_STRATEGY);
    need(code == Z_OK, ErrorCode::ResourceRefused, c.phase, "Cannot initialize bounded zlib encoder"); codec.initialized = true;
    std::vector<std::byte> output(memorySize(compressBound(static_cast<uLong>(raw.size()))));
    auto &z = codec.stream;
    while (true) {
        c.check();
        if (z.avail_in == 0 && z.total_in < raw.size()) {
            z.next_in = reinterpret_cast<Bytef *>(const_cast<std::byte *>(raw.data() + z.total_in));
            z.avail_in = static_cast<uInt>(std::min<U64>(16384, raw.size() - z.total_in));
        }
        need(z.total_out < output.size(), ErrorCode::ResourceRefused, c.phase, "Compressed bound exceeded");
        z.next_out = reinterpret_cast<Bytef *>(output.data() + z.total_out);
        z.avail_out = static_cast<uInt>(std::min<U64>(16384, output.size() - z.total_out));
        const bool last = z.total_in + z.avail_in == raw.size();
        code = deflate(&z, last ? Z_FINISH : Z_NO_FLUSH);
        c.stats.codecPeak = std::max(c.stats.codecPeak, codec.peak);
        if (code == Z_STREAM_END) break;
        need(code == Z_OK, code == Z_MEM_ERROR ? ErrorCode::ResourceRefused : ErrorCode::IO, c.phase, "zlib encode failed");
    }
    output.resize(z.total_out); c.check(); return output;
}
std::vector<std::byte> decompress(Bytes compressed, U64 expected, Context &c)
{
    Codec codec(false, c.policy.codecBytes);
    int code = inflateInit2(&codec.stream, 15);
    need(code == Z_OK, ErrorCode::ResourceRefused, c.phase, "Cannot initialize bounded zlib decoder"); codec.initialized = true;
    std::vector<std::byte> raw(memorySize(expected)); std::byte overflow{};
    auto &z = codec.stream;
    while (true) {
        c.check();
        if (!z.avail_in && z.total_in < compressed.size()) {
            z.next_in = reinterpret_cast<Bytef *>(const_cast<std::byte *>(compressed.data() + z.total_in));
            z.avail_in = static_cast<uInt>(std::min<U64>(16384, compressed.size() - z.total_in));
        }
        const U64 beforeIn = z.total_in, beforeOut = z.total_out;
        z.next_out = reinterpret_cast<Bytef *>(z.total_out < expected ? raw.data() + z.total_out : &overflow);
        z.avail_out = static_cast<uInt>(z.total_out < expected ? std::min<U64>(16384, expected - z.total_out) : 1);
        code = inflate(&z, Z_NO_FLUSH);
        c.stats.codecPeak = std::max(c.stats.codecPeak, codec.peak);
        need(z.total_out <= expected, ErrorCode::CorruptData, c.phase, "Oversized decoded stream");
        if (code == Z_STREAM_END) break;
        need(code == Z_OK && (z.total_in != beforeIn || z.total_out != beforeOut),
             code == Z_MEM_ERROR ? ErrorCode::ResourceRefused : ErrorCode::CorruptData, c.phase, "Invalid/truncated/dictionary zlib stream");
    }
    need(z.total_in == compressed.size() && z.total_out == expected,
         ErrorCode::CorruptData, c.phase, "Trailing stream bytes or decoded size mismatch");
    c.check(); return raw;
}
void cleanupFile(std::unique_ptr<QTemporaryFile> &file, Diagnostic diagnostic, void *context) noexcept
{
    if (!file) return;
    if (!file->remove() && QFileInfo::exists(file->fileName()) && diagnostic) {
        auto e = error(ErrorCode::CleanupFailed, Phase::Cleanup, "Cannot remove owned scratch file");
        e.deviceCode = static_cast<int>(file->error()); e.detail = file->errorString().left(256); diagnostic(e, context);
    }
    file.reset(); // autoRemove retries, never suppresses the earlier diagnostic
}
} // namespace

DatasetId DatasetId::create() { return {cache::CacheId::create().bytes}; }
bool DatasetId::isNull() const noexcept { return std::all_of(bytes.begin(), bytes.end(), [](auto b) { return b == std::byte{}; }); }
ResourcePolicy initialResourcePolicy() noexcept
{
    return {8 * 1024 * 1024, 1024 * 1024, 1024 * 1024,
            4ULL * 1024 * 1024 * 1024, 4ULL * 1024 * 1024 * 1024, 8ULL * 1024 * 1024 * 1024,
            5, 10'000'000, 100'000'000, raster::ValidationPolicy{raster::initialBoundaryCheckBudget}};
}
Result<Shape> tileShape(U64 width, U64 height, std::uint32_t side, TileKey key)
{
    return attempt<Shape>(Phase::Metadata, [&] { return shape(width, height, side, key); });
}
PendingDataset::PendingDataset(cache::PublishedFile file, Metadata metadata, Diagnostic diagnostic, void *context)
    : file_(std::move(file)), metadata_(std::move(metadata)), diagnostic_(diagnostic), diagnosticContext_(context) {}
PendingDataset::PendingDataset(PendingDataset &&other) noexcept
    : file_(std::move(other.file_)), metadata_(std::move(other.metadata_)), diagnostic_(other.diagnostic_), diagnosticContext_(other.diagnosticContext_) {}
PendingDataset &PendingDataset::operator=(PendingDataset &&other) noexcept
{
    if (this != &other) { discard(); file_ = std::move(other.file_); metadata_ = std::move(other.metadata_);
        diagnostic_ = other.diagnostic_; diagnosticContext_ = other.diagnosticContext_; }
    return *this;
}
PendingDataset::~PendingDataset() { discard(); }
QString PendingDataset::path() const noexcept { return file_ ? file_->path() : QString{}; }
QString PendingDataset::releaseToOwner() noexcept { auto p = file_ ? file_->releaseToOwner() : QString{}; file_.reset(); return p; }
void PendingDataset::discard() noexcept
{
    auto p = releaseToOwner(); if (p.isEmpty()) return;
    QFile f(p);
    if (!f.remove() && diagnostic_) {
        auto e = error(ErrorCode::CleanupFailed, Phase::Cleanup, "Cannot remove discarded density dataset");
        e.deviceCode = static_cast<int>(f.error()); e.detail = f.errorString().left(256); diagnostic_(e, diagnosticContext_);
    }
}

namespace {
struct Guard {
    std::atomic<bool> &active;
    Guard(std::atomic<bool> &flag, Phase phase) : active(flag)
    {
        bool expected = false;
        if (!active.compare_exchange_strong(expected, true)) fail(ErrorCode::InvalidState, phase, "Concurrent/reentrant operation");
    }
    ~Guard() { active.store(false); }
};
}
struct Reader::Impl {
    QFile file;
    Context c;
    std::optional<Header> header;
    std::atomic<bool> active = false;
    explicit Impl(const QString &path, ResourcePolicy policy) : file(path), c{policy} {}
    void initialize()
    {
        c.phase = Phase::Open; c.check(); budget(1, c.policy.openFiles, c.phase, "Reader handle budget");
        if (!file.open(QIODevice::ReadOnly)) deviceFailure(file, c.phase);
        const auto actualSize = cache::file::size(file);
        if (!actualSize) cacheStatus(actualSize.error(), c.phase);
        need(actualSize.value() >= headerSize + footerSize, ErrorCode::IncompleteDataset, c.phase, "Truncated density envelope");
        std::array<std::byte, headerSize> bytes{}; c.read(file, 0, bytes);
        header = decodeHeader(bytes, c); const auto &h = *header;
        need(actualSize.value() >= h.fileSize, ErrorCode::IncompleteDataset, c.phase, "Truncated density file");
        need(actualSize.value() == h.fileSize, ErrorCode::CorruptData, c.phase, "Unexpected trailing file bytes");
        c.stats.bufferReserve = bufferReserve(h.metadata.tileSide);
        budget(c.stats.bufferReserve, c.policy.bufferBytes, c.phase, "Reader working buffers");
        budget(h.tiles, c.policy.indexRecords, Phase::Index, "Structural index record budget");
        std::array<std::byte, footerSize> footer{}; c.read(file, h.footerOffset, footer); validateFooter(footer, h);
        c.phase = Phase::Index;
        U64 end = headerSize, total = 0; std::optional<TileKey> previous; std::uint32_t checksum = 0;
        for (U64 i = 0; i < h.tiles; ++i) {
            c.check(); std::array<std::byte, entrySize> wire{}; c.read(file, h.indexOffset + i * entrySize, wire); c.stats.indexReads = add(c.stats.indexReads, 1);
            checksum = crc(wire, checksum); auto e = decode(wire); validateEntry(e);
            need(!previous || less(*previous, e.key), ErrorCode::CorruptData, c.phase, "Duplicate/unordered index key");
            need(e.offset == end, ErrorCode::CorruptData, c.phase, "Payload gap/overlap");
            end = add(end, e.compressed, ErrorCode::CorruptData, c.phase);
            total = add(total, e.total, ErrorCode::CorruptData, c.phase); previous = e.key;
        }
        need(end == h.indexOffset && total == h.total && checksum == h.indexCrc,
             ErrorCode::CorruptData, c.phase, "Index coverage/total/checksum mismatch");
        c.stats.physicalTiles = h.tiles; c.stats.total = h.total; c.stats.outputBytes = h.fileSize; c.check();
    }
    void validateEntry(const Entry &e)
    {
        const auto &h = *header;
        auto expected = tileShape(h.metadata.mapping.width(), h.metadata.mapping.height(), h.metadata.tileSide, e.key);
        need(static_cast<bool>(expected), ErrorCode::CorruptData, c.phase, "Index key outside logical bounds");
        need(e.encoding == 1 || e.encoding == 2, ErrorCode::UnsupportedFormat, c.phase, "Unsupported sample encoding");
        const U64 raw = multiply(multiply(expected.value().width, expected.value().height), e.encoding == 1 ? 4 : 8);
        need(e.shape == expected.value() && e.raw == raw && e.total >= e.maximum && e.maximum != 0
            && (e.encoding == 1 ? e.maximum <= UINT32_MAX : e.maximum > UINT32_MAX),
             ErrorCode::CorruptData, c.phase, "Index shape/sample/summary mismatch");
        need(e.compressed > 0 && e.compressed <= compressBound(static_cast<uLong>(raw)) && e.offset >= headerSize
            && cache::checked::range(e.offset, e.compressed, h.indexOffset),
             ErrorCode::CorruptData, c.phase, "Invalid compressed tile range");
    }
    Entry entry(U64 ordinal)
    {
        need(ordinal < header->tiles, ErrorCode::CorruptData, c.phase, "Index ordinal outside range");
        std::array<std::byte, entrySize> b{}; c.read(file, header->indexOffset + ordinal * entrySize, b);
        c.stats.indexReads = add(c.stats.indexReads, 1); auto e = decode(b); validateEntry(e); return e;
    }
    Tile tile(const Entry &e)
    {
        try {
            validateEntry(e);
            std::vector<std::byte> compressed(memorySize(e.compressed)); c.read(file, e.offset, compressed);
            auto raw = decompress(compressed, e.raw, c);
            need(crc(raw) == e.checksum, ErrorCode::CorruptData, c.phase, "Decoded tile checksum mismatch");
            const auto n = multiply(e.shape.width, e.shape.height);
            Tile t{e.key, e.shape, std::vector<U64>(memorySize(n))}; U64 total = 0, max = 0;
            for (std::size_t i = 0; i < t.counts.size(); ++i) {
                if (i % 4096 == 0) c.check();
                const U64 value = e.encoding == 1 ? *getU32(raw, i * 4) : *getU64(raw, i * 8);
                total = add(total, value, ErrorCode::CorruptData, c.phase); max = std::max(max, value); t.counts[i] = value;
            }
            need(total == e.total && max == e.maximum && max != 0,
                 ErrorCode::CorruptData, c.phase, "Decoded summary/canonical zero omission mismatch");
            c.stats.decodedTiles = add(c.stats.decodedTiles, 1); c.check(); return t;
        } catch (Failure &f) { f.error.tile = e.key; if (!f.error.offset) f.error.offset = e.offset; throw; }
    }
    Tile read(TileKey key)
    {
        const auto &m = header->metadata.mapping;
        const auto dimensions = shape(m.width(), m.height(), header->metadata.tileSide, key);
        U64 first = 0, last = header->tiles;
        while (first < last) {
            const U64 mid = first + (last - first) / 2; auto e = entry(mid);
            if (less(e.key, key)) first = mid + 1; else last = mid;
        }
        if (first < header->tiles) { auto e = entry(first); if (e.key == key) return tile(e); }
        Tile zero{key, dimensions, std::vector<U64>(memorySize(multiply(dimensions.width, dimensions.height)), 0)};
        c.check(); return zero;
    }
    void iterate(Reader::Sink sink, void *context)
    {
        U64 total = 0;
        for (U64 i = 0; i < header->tiles; ++i) {
            c.check(); auto e = entry(i); auto t = tile(e); total = add(total, e.total, ErrorCode::CorruptData, c.phase);
            if (sink) { auto result = sink(t, context); if (!result) {
                auto failure = result.error(); failure.detail = failure.detail.left(256); throw Failure{std::move(failure)};
            } }
        }
        need(total == header->total, ErrorCode::CorruptData, c.phase, "Full validation total mismatch"); c.check();
    }
    void begin(Phase phase, CancelCheck cancel, void *context)
    {
        c.phase = phase; c.cancel = cancel; c.cancelContext = context; c.stats.workUnits = 0; c.check();
    }
};
Reader::Reader(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Reader::~Reader() = default;
Result<std::unique_ptr<Reader>> Reader::openReader(const QString &path, ResourcePolicy policy, CancelCheck cancel, void *context)
{
    return attempt<std::unique_ptr<Reader>>(Phase::Open, [&] {
        auto impl = std::make_unique<Impl>(path, policy); impl->c.cancel = cancel; impl->c.cancelContext = context;
        impl->initialize(); return std::unique_ptr<Reader>(new Reader(std::move(impl)));
    });
}
const Metadata &Reader::metadata() const noexcept { return impl_->header->metadata; }
const Statistics &Reader::statistics() const noexcept { return impl_->c.stats; }
Result<Tile> Reader::readTile(TileKey key, CancelCheck cancel, void *context)
{
    return attempt<Tile>(Phase::Read, [&] { Guard guard(impl_->active, Phase::Read); impl_->begin(Phase::Read, cancel, context);
        auto tile = impl_->read(key); impl_->c.check(); return tile; });
}
Status Reader::forEachTile(Sink sink, void *sinkContext, CancelCheck cancel, void *context)
{
    return attempt<std::monostate>(Phase::Read, [&] { Guard guard(impl_->active, Phase::Read);
        need(sink != nullptr, ErrorCode::InvalidInput, Phase::Read, "Missing streaming consumer");
        impl_->begin(Phase::Read, cancel, context); impl_->iterate(sink, sinkContext); return std::monostate{}; });
}
Status Reader::fullValidate(CancelCheck cancel, void *context)
{
    return attempt<std::monostate>(Phase::Validate, [&] { Guard guard(impl_->active, Phase::Validate);
        impl_->begin(Phase::Validate, cancel, context); impl_->iterate(nullptr, nullptr); return std::monostate{}; });
}

struct Writer::Impl {
    Context c;
    Metadata metadata;
    QString finalPath;
    std::unique_ptr<cache::StagedFile> staged;
    std::array<std::unique_ptr<QTemporaryFile>, 3> scratch;
    std::array<U64, 4> sizes{}; // spool, two descriptor tapes, candidate
    U64 candidatePosition = 0, submissions = 0, physical = 0, total = 0;
    bool terminal = false;
    std::atomic<bool> active = false;
    Diagnostic diagnostic;
    void *diagnosticContext;
    Impl(Metadata m, ResourcePolicy policy, CancelCheck cancel, void *context, Diagnostic d, void *dc)
        : c{policy}, metadata(std::move(m)), diagnostic(d), diagnosticContext(dc)
    { c.cancel = cancel; c.cancelContext = context; }
    ~Impl() { cleanup(); }
    void cleanup() noexcept
    {
        for (auto &file : scratch) cleanupFile(file, diagnostic, diagnosticContext);
        if (staged) {
            const auto result = staged->discard();
            if (!result && diagnostic) {
                auto e = error(ErrorCode::CleanupFailed, Phase::Cleanup, "Cannot remove owned staged candidate");
                e.deviceCode = result.error().deviceCode; e.detail = result.error().detail.left(256); diagnostic(e, diagnosticContext);
            }
            staged.reset();
        }
    }
    void reserve(std::size_t which, U64 length)
    {
        need(cache::checked::fileOffset(length).has_value(), ErrorCode::ResourceRefused, c.phase, "File size unrepresentable");
        auto planned = sizes; planned[which] = length;
        const U64 scratchBytes = add(add(planned[0], planned[1]), planned[2]);
        const U64 combined = add(scratchBytes, planned[3]);
        budget(scratchBytes, c.policy.scratchBytes, c.phase, "Scratch byte budget");
        budget(planned[3], c.policy.outputBytes, c.phase, "Output byte budget");
        budget(combined, c.policy.combinedDiskBytes, c.phase, "Combined spool/tapes/candidate byte budget");
        sizes = planned;
        c.stats.scratchPeak = std::max(c.stats.scratchPeak, scratchBytes);
        c.stats.combinedDiskPeak = std::max(c.stats.combinedDiskPeak, combined);
    }
    void candidate(Bytes b)
    {
        const U64 end = add(candidatePosition, b.size()); reserve(3, std::max(sizes[3], end));
        for (std::size_t done = 0; done < b.size();) {
            c.check(); auto n = std::min<std::size_t>(ioChunk, b.size() - done);
            cacheStatus(staged->write(b.subspan(done, n)), c.phase); c.stats.writtenBytes = add(c.stats.writtenBytes, n); done += n;
        }
        candidatePosition = end;
    }
    void seekCandidate(U64 offset) { c.check(); cacheStatus(staged->seek(offset), c.phase); candidatePosition = offset; }
    void initialize(const QString &path, const QString &parent)
    {
        c.check(); need(!metadata.id.isNull() && !metadata.source.isNull(), ErrorCode::InvalidInput, c.phase, "Null UUID");
        need(metadata.contract == BuildContract::SuppliedTiles || metadata.contract == BuildContract::CompletePointCache,
             ErrorCode::InvalidInput, c.phase, "Invalid build contract");
        need(metadata.tileSide == 256 || metadata.tileSide == 512, ErrorCode::InvalidInput, c.phase, "Unsupported tile side");
        metadata.mapping = mapping(metadata.mapping.minimum(), metadata.mapping.maximum(), metadata.mapping.cell(), c);
        c.stats.bufferReserve = bufferReserve(metadata.tileSide);
        budget(c.stats.bufferReserve, c.policy.bufferBytes, c.phase, "Writer working buffers");
        budget(1, c.policy.codecBytes, c.phase, "Codec memory required");
        budget(sizeof(Entry), c.policy.sortBytes, c.phase, "Sort needs at least one descriptor");
        budget(5, c.policy.openFiles, c.phase, "Writer/readback handle budget");
        budget(headerSize + footerSize, c.policy.outputBytes, c.phase, "Minimum candidate output");
        budget(headerSize + footerSize, c.policy.combinedDiskBytes, c.phase, "Minimum combined disk budget");
        budget(entrySize, c.policy.scratchBytes, c.phase, "Scratch descriptor required");
        need(QDir(parent).exists(), ErrorCode::InvalidInput, c.phase, "Scratch parent does not exist");
        finalPath = QFileInfo(path).absoluteFilePath();
        need(!QFileInfo::exists(finalPath), ErrorCode::IO, Phase::Publish, "Destination already exists");
        auto file = cache::StagedFile::create(path);
        if (!file) cacheStatus(file.error(), Phase::Open);
        staged = std::move(file.value());
        for (auto &scratchFile : scratch) {
            c.check(); scratchFile = std::make_unique<QTemporaryFile>(QDir(parent).filePath(QStringLiteral(".pc2d-density-XXXXXX.tmp")));
            if (!scratchFile->open()) deviceFailure(*scratchFile, Phase::Open);
        }
        c.check();
    }
    void append(TileKey key, Shape supplied, std::span<const U64> values)
    {
        c.phase = Phase::Ingest; c.check();
        const auto expected = shape(metadata.mapping.width(), metadata.mapping.height(), metadata.tileSide, key);
        need(supplied == expected && values.size() == multiply(expected.width, expected.height),
             ErrorCode::InvalidInput, c.phase, "Tile shape/count length mismatch");
        U64 sum = 0, max = 0;
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (i % 4096 == 0) c.check();
            sum = add(sum, values[i], ErrorCode::InvalidInput, c.phase); max = std::max(max, values[i]);
        }
        const U64 nextTotal = add(total, sum, ErrorCode::InvalidInput, c.phase);
        const U64 nextSubmissions = add(submissions, 1);
        budget(nextSubmissions, c.policy.indexRecords, c.phase, "Submitted descriptor budget (including zero tiles)");
        Entry e{key, sizes[0], 0, 0, sum, max, 0, max > UINT32_MAX ? 2U : 1U, expected};
        if (max) {
            c.phase = Phase::Codec;
            const auto sample = e.encoding == 1 ? 4U : 8U;
            e.raw = multiply(values.size(), sample);
            std::vector<std::byte> raw(memorySize(e.raw));
            for (std::size_t i = 0; i < values.size(); ++i) {
                if (i % 4096 == 0) c.check();
                if (sample == 4) putU32(raw, i * 4, static_cast<std::uint32_t>(values[i])); else putU64(raw, i * 8, values[i]);
            }
            e.checksum = crc(raw); auto compressed = compress(raw, c); e.compressed = compressed.size();
            const U64 end = add(sizes[0], e.compressed);
            // Refuse impossible final output before extending the spool.
            budget(add(add(headerSize + footerSize, end), multiply(add(physical, 1), entrySize)),
                   c.policy.outputBytes, c.phase, "Future completed output budget");
            reserve(0, end); c.write(*scratch[0], e.offset, compressed); ++physical;
        }
        c.phase = Phase::Ingest;
        const U64 descriptorOffset = multiply(submissions, entrySize); reserve(1, add(descriptorOffset, entrySize));
        const auto wire = encode(e); c.write(*scratch[1], descriptorOffset, wire);
        submissions = nextSubmissions; total = nextTotal;
        c.stats.submissions = submissions; c.stats.physicalTiles = physical; c.stats.total = total; c.check();
    }
    Entry descriptor(std::size_t tape, U64 ordinal)
    {
        std::array<std::byte, entrySize> wire{}; c.read(*scratch[tape], multiply(ordinal, entrySize), wire); return decode(wire);
    }
    void descriptor(std::size_t tape, U64 ordinal, const Entry &e)
    {
        const U64 offset = multiply(ordinal, entrySize); reserve(tape, std::max(sizes[tape], add(offset, entrySize)));
        const auto wire = encode(e); c.write(*scratch[tape], offset, wire);
    }
    void truncate(std::size_t tape)
    {
        c.check(); if (!scratch[tape]->resize(0)) deviceFailure(*scratch[tape], c.phase); reserve(tape, 0);
    }
    std::size_t sort()
    {
        QElapsedTimer timer; timer.start(); c.phase = Phase::Sort; c.check();
        const U64 capacity = std::min<U64>(maximumRun, c.policy.sortBytes / sizeof(Entry));
        budget(sizeof(Entry), c.policy.sortBytes, c.phase, "Sort descriptor reserve");
        const U64 run = std::min(capacity, std::max<U64>(1, submissions));
        c.stats.sortReserve = multiply(run, sizeof(Entry)); std::vector<Entry> buffer(memorySize(run));
        for (U64 start = 0; start < submissions;) {
            c.check(); const U64 n = std::min(run, submissions - start);
            for (U64 i = 0; i < n; ++i) buffer[memorySize(i)] = descriptor(1, start + i);
            std::sort(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(n), [](const Entry &a, const Entry &b) { return less(a.key, b.key); });
            c.check();
            for (U64 i = 0; i < n; ++i) descriptor(2, start + i, buffer[memorySize(i)]);
            start += n;
        }
        // Free sort array before merge/copy/readback buffers coexist.
        std::vector<Entry>().swap(buffer);
        std::size_t source = 2, target = 1; U64 width = run;
        while (width < submissions) {
            c.phase = Phase::Merge; c.check(); truncate(target);
            for (U64 start = 0; start < submissions;) {
                const U64 middle = start + std::min(width, submissions - start);
                const U64 end = middle + std::min(width, submissions - middle);
                U64 left = start, right = middle, out = start;
                std::optional<Entry> a, b;
                while (out < end) {
                    c.check(); if (!a && left < middle) a = descriptor(source, left++);
                    if (!b && right < end) b = descriptor(source, right++);
                    if (a && (!b || less(a->key, b->key))) { descriptor(target, out++, *a); a.reset(); }
                    else { descriptor(target, out++, *b); b.reset(); }
                }
                start = end;
            }
            std::swap(source, target); ++c.stats.mergePasses;
            width = width >= submissions - width ? submissions : width * 2;
        }
        c.stats.sortNanoseconds = static_cast<U64>(timer.nsecsElapsed()); c.check(); return source;
    }
    Header assemble(std::size_t tape)
    {
        Header h{metadata}; h.tiles = physical; h.total = total; h.payload = sizes[0];
        h.indexOffset = add(headerSize, h.payload); h.indexLength = multiply(physical, entrySize);
        h.footerOffset = add(h.indexOffset, h.indexLength); h.fileSize = add(h.footerOffset, footerSize);
        need(metadata.contract != BuildContract::CompletePointCache || total == metadata.sourceMeasurements,
             ErrorCode::InvalidInput, Phase::Seal, "Complete Point Cache count contract mismatch");
        QElapsedTimer timer; timer.start(); c.phase = Phase::Copy; c.check();
        std::array<std::byte, headerSize> placeholder{}; candidate(placeholder);
        std::array<std::byte, ioChunk> buffer{}; std::optional<TileKey> previous;
        for (U64 i = 0; i < submissions; ++i) {
            c.check(); const auto e = descriptor(tape, i);
            need(!previous || less(*previous, e.key), ErrorCode::InvalidInput, c.phase, "Duplicate submitted tile key"); previous = e.key;
            for (U64 done = 0; done < e.compressed;) {
                const auto n = std::min<U64>(ioChunk, e.compressed - done);
                c.read(*scratch[0], add(e.offset, done), MutableBytes(buffer).first(memorySize(n)));
                candidate(Bytes(buffer).first(memorySize(n))); done += n;
            }
        }
        need(candidatePosition == h.indexOffset, ErrorCode::InvalidState, c.phase, "Internal payload size mismatch");
        U64 offset = headerSize;
        for (U64 i = 0; i < submissions; ++i) {
            c.check(); auto e = descriptor(tape, i); if (!e.maximum) continue;
            e.offset = offset; offset = add(offset, e.compressed);
            const auto wire = encode(e); h.indexCrc = crc(wire, h.indexCrc); candidate(wire);
        }
        need(candidatePosition == h.footerOffset && offset == h.indexOffset, ErrorCode::InvalidState, c.phase, "Internal index size mismatch");
        c.stats.copyNanoseconds = static_cast<U64>(timer.nsecsElapsed());
        c.phase = Phase::Seal; c.check(); const auto header = encodeHeader(h); h.headerCrc = *getU32(header, 236);
        const auto footer = encodeFooter(h); candidate(footer); seekCandidate(0); candidate(header);
        c.phase = Phase::Flush; c.check(); cacheStatus(staged->flush(), c.phase); c.stats.outputBytes = h.fileSize; return h;
    }
    std::optional<Error> validationError;
    cache::Status validateCandidate()
    {
        auto result = attempt<std::monostate>(Phase::Readback, [&] {
            c.phase = Phase::Readback; c.check(); QElapsedTimer timer; timer.start();
            auto policy = c.policy; policy.workUnits -= c.stats.workUnits;
            auto opened = Reader::openReader(staged->temporaryPath(), policy, c.cancel, c.cancelContext);
            if (!opened) throw Failure{opened.error()};
            auto reader = std::move(opened.value());
            const auto openWork = reader->statistics().workUnits;
            budget(add(c.stats.workUnits, openWork), c.policy.workUnits, c.phase, "Readback open work budget"); c.stats.workUnits += openWork;
            const auto remaining = c.policy.workUnits - c.stats.workUnits;
            // Reader's per-operation budget is the remaining writer work budget.
            reader->impl_->c.policy.workUnits = remaining;
            auto checked = reader->fullValidate(c.cancel, c.cancelContext);
            if (!checked) throw Failure{checked.error()};
            c.stats.workUnits = add(c.stats.workUnits, reader->statistics().workUnits);
            c.stats.readBytes = add(c.stats.readBytes, reader->statistics().readBytes);
            c.stats.codecPeak = std::max(c.stats.codecPeak, reader->statistics().codecPeak);
            c.stats.mappingNanoseconds = add(c.stats.mappingNanoseconds, reader->statistics().mappingNanoseconds);
            c.stats.readbackNanoseconds = static_cast<U64>(timer.nsecsElapsed());
            c.phase = Phase::Publish; c.check(); return std::monostate{};
        });
        if (!result) { validationError = result.error(); return cache::makeError(cache::ErrorCode::CorruptCache,
            cache::Phase::Validation, cache::Operation::Read, result.error().detail); }
        return cache::success();
    }
};
Writer::Writer(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Writer::~Writer() = default;
const Statistics &Writer::statistics() const noexcept { return impl_->c.stats; }
void Writer::setFaultHook(Status (*hook)(Phase, void *) noexcept, void *context)
{ impl_->c.hook = hook; impl_->c.hookContext = context; }
Result<std::unique_ptr<Writer>> Writer::create(const QString &path, Metadata metadata, ResourcePolicy policy,
    const QString &parent, CancelCheck cancel, void *context, Diagnostic diagnostic, void *diagnosticContext)
{
    return attempt<std::unique_ptr<Writer>>(Phase::Metadata, [&] {
        auto impl = std::make_unique<Impl>(std::move(metadata), policy, cancel, context, diagnostic, diagnosticContext);
        impl->initialize(path, parent); return std::unique_ptr<Writer>(new Writer(std::move(impl)));
    });
}
Status Writer::addTile(TileKey key, Shape shape, std::span<const U64> counts)
{
    return attempt<std::monostate>(Phase::Ingest, [&] {
        Guard guard(impl_->active, Phase::Ingest);
        need(!impl_->terminal, ErrorCode::InvalidState, Phase::Ingest, "Writer is terminal");
        auto result = attempt<std::monostate>(Phase::Ingest, [&] { impl_->append(key, shape, counts); return std::monostate{}; });
        if (!result) { impl_->terminal = true; impl_->cleanup(); auto e = result.error(); e.tile = key; throw Failure{std::move(e)}; }
        return std::monostate{};
    });
}
Result<PendingDataset> Writer::finish()
{
    return attempt<PendingDataset>(Phase::Seal, [&] {
        Guard guard(impl_->active, Phase::Seal);
        need(!impl_->terminal, ErrorCode::InvalidState, Phase::Seal, "Writer is terminal"); impl_->terminal = true;
        auto result = attempt<PendingDataset>(Phase::Seal, [&] {
            impl_->c.check(); auto tape = impl_->sort(); auto h = impl_->assemble(tape);
            auto validator = [](QIODevice &, U64, void *context) -> cache::Status { return static_cast<Impl *>(context)->validateCandidate(); };
            auto published = impl_->staged->publish(h.fileSize, +validator, impl_.get());
            if (!published) {
                if (impl_->validationError) throw Failure{*impl_->validationError};
                cacheStatus(published.error(), Phase::Publish);
            }
            impl_->staged.reset();
            PendingDataset pending(std::move(published.value()), impl_->metadata, impl_->diagnostic, impl_->diagnosticContext);
            impl_->c.phase = Phase::Publish; impl_->c.check();
            impl_->cleanup(); return pending;
        });
        if (!result) { impl_->cleanup(); throw Failure{result.error()}; }
        return std::move(result.value());
    });
}
} // namespace cadcontour::density
