#include "formats.h"
#include <QFileInfo>
#include <tiffio.h>
#include <zlib.h>
#include <algorithm>
#include <limits>

namespace density::formats {
U64 columns(const Spec &s) { return 1 + (s.width - 1) / s.side; }
U64 logicalTiles(const Spec &s) {
    tileCount(s); // Validate the shared Spec before any division or allocation.
    return multiply(columns(s), 1 + (s.height - 1) / s.side);
}
U64 rasterOrdinal(const Spec &s, TileKey k) {
    stored(s,k);
    return add(multiply(k.y, columns(s)), k.x);
}
Tile blank(const Spec &s, TileKey k) {
    rasterOrdinal(s,k);
    Tile t{k, static_cast<std::uint32_t>(std::min<U64>(s.side, s.width - k.x*s.side)),
        static_cast<std::uint32_t>(std::min<U64>(s.side, s.height - k.y*s.side)), {}};
    t.counts.resize(std::size_t(t.width)*t.height);
    return t;
}
void validateTile(const Spec &s, const Tile &t, TileKey k) {
    rasterOrdinal(s,k);
    require(t.key == k && t.width == std::min<U64>(s.side,s.width-k.x*s.side)
        && t.height == std::min<U64>(s.side,s.height-k.y*s.side)
        && t.counts.size() == U64(t.width)*t.height, "source returned invalid tile");
    if (!s.wide) for (auto n : t.counts) require(n <= UINT32_MAX, "uint32 count overflow");
}
Plan storagePlan(const Spec &s, bool tiff) {
    const auto count = tiff ? logicalTiles(s) : tileCount(s);
    if (tiff && (s.width > UINT32_MAX || s.height > UINT32_MAX || count > UINT32_MAX
        || s.side % 16)) throw Failure("unsupported", "TIFF requires uint32 dimensions/tile count and tile sides divisible by 16");
    const U64 tile = multiply(multiply(s.side,s.side),s.wide ? 8 : 4);
    // Includes Tile<uint64>, packed/decoded buffers, Deflate state, index duplication,
    // libtiff arrays and process/runtime reserve. Not an allocator trace or OS cap.
    return {add(48*MiB,add(multiply(tile,8),multiply(count,128))), 0,
        add(8*MiB,add(multiply(count,128),multiply(count,compressBound(static_cast<uLong>(tile)))))};
}
QJsonObject versions() {
    return {{"zlibHeaders",ZLIB_VERSION},{"zlibRuntime",zlibVersion()},
        {"libtiff",QString::fromLatin1(TIFFGetVersion())},{"deflateLevel",6},
        {"predictor",1},{"tiffCodecConfigured",bool(TIFFIsCODECConfigured(COMPRESSION_ADOBE_DEFLATE))}};
}
TrackedFile::TrackedFile(const QString &path, bool write) : file(path) {
    if (!file.open((write ? QIODevice::WriteOnly | QIODevice::NewOnly : QIODevice::ReadOnly) | QIODevice::Unbuffered))
        throw Failure("io_error",file.errorString());
}
void TrackedFile::seek(U64 offset) {
    if (offset > U64(std::numeric_limits<qint64>::max()) || !file.seek(static_cast<qint64>(offset)))
        throw Failure("io_error","invalid file offset or seek failure");
}
QByteArray TrackedFile::read(U64 offset, U64 bytes) {
    require(offset <= U64(file.size()) && bytes <= U64(file.size())-offset
        && bytes <= U64(std::numeric_limits<qsizetype>::max()), "read outside file");
    seek(offset); QByteArray b(static_cast<qsizetype>(bytes),Qt::Uninitialized);
    qint64 done = 0;
    while (done < b.size()) {
        const auto n = file.read(b.data()+done,b.size()-done); ++io.reads;
        if (n <= 0) throw Failure("io_error","short container read");
        io.readBytes = add(io.readBytes,U64(n)); done += n;
    }
    return b;
}
void TrackedFile::write(const QByteArray &b) {
    qint64 done = 0;
    while (done < b.size()) {
        const auto n = file.write(b.constData()+done,b.size()-done); ++io.writes;
        if (n <= 0) throw Failure("io_error","short container write");
        io.writeBytes = add(io.writeBytes,U64(n)); done += n;
    }
}
void TrackedFile::finish() {
    if (!file.flush()) throw Failure("io_error",file.errorString());
    file.close();
    if (file.error() != QFileDevice::NoError) throw Failure("io_error",file.errorString());
}
} // namespace density::formats
