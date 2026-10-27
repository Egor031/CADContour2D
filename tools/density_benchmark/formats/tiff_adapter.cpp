#include "formats.h"
#include <tiffio.h>
#include <zlib.h>
#include <QtEndian>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>

namespace density::formats {
namespace {
struct Client {
    TrackedFile file;
    char error[512]{};
    Client(const QString &path, bool write) : file(path,write) {}
    static tmsize_t read(thandle_t handle, void *buffer, tmsize_t size) {
        auto &c = *static_cast<Client *>(handle);
        if (size < 0) return -1;
        const auto n = c.file.file.read(static_cast<char *>(buffer),size); ++c.file.io.reads;
        if (n > 0) c.file.io.readBytes += U64(n);
        return n;
    }
    static tmsize_t write(thandle_t handle, void *buffer, tmsize_t size) {
        auto &c = *static_cast<Client *>(handle);
        if (size < 0) return -1;
        const auto n = c.file.file.write(static_cast<const char *>(buffer),size); ++c.file.io.writes;
        if (n > 0) c.file.io.writeBytes += U64(n);
        return n;
    }
    static toff_t seek(thandle_t handle, toff_t offset, int origin) {
        auto &f = static_cast<Client *>(handle)->file.file;
        U64 base = origin == SEEK_SET ? 0 : origin == SEEK_CUR ? U64(f.pos()) : U64(f.size());
        // libtiff uses absolute positions here; reject wrapping relative seeks.
        if (offset > U64(INT64_MAX)-base || !f.seek(qint64(base+offset))) return toff_t(-1);
        return toff_t(f.pos());
    }
    static int close(thandle_t) { return 0; } // The owner checks flush/close explicitly.
    static toff_t size(thandle_t handle) { return toff_t(static_cast<Client *>(handle)->file.file.size()); }
    static int map(thandle_t,void **,toff_t *) { return 0; }
    static void unmap(thandle_t,void *,toff_t) {}
    static int message(TIFF *, void *data, const char *, const char *format, va_list args) {
        auto &c = *static_cast<Client *>(data);
        std::vsnprintf(c.error,sizeof(c.error),format,args); return 1;
    }
};
struct TiffHandle {
    Client client;
    std::unique_ptr<TIFF,decltype(&TIFFClose)> tif{nullptr,TIFFClose};
    TiffHandle(const QString &path, bool write, bool big, U64 budget) : client(path,write) {
        require(budget && budget <= U64(INT64_MAX),"invalid libtiff memory budget");
        std::unique_ptr<TIFFOpenOptions,decltype(&TIFFOpenOptionsFree)> options(TIFFOpenOptionsAlloc(),TIFFOpenOptionsFree);
        if (!options) throw Failure("resource_refused","TIFF options allocation failed");
        TIFFOpenOptionsSetMaxSingleMemAlloc(options.get(),static_cast<tmsize_t>(budget));
        TIFFOpenOptionsSetMaxCumulatedMemAlloc(options.get(),static_cast<tmsize_t>(budget));
        TIFFOpenOptionsSetErrorHandlerExtR(options.get(),Client::message,&client);
        TIFFOpenOptionsSetWarningHandlerExtR(options.get(),Client::message,&client);
        tif.reset(TIFFClientOpenExt("density-06b",write ? (big ? "w8l" : "wl") : "rm",
            &client,Client::read,Client::write,Client::seek,Client::close,Client::size,Client::map,Client::unmap,options.get()));
        ok(bool(tif),"TIFF open failed");
    }
    void ok(bool value, const char *context) const {
        if (!value || client.error[0]) throw Failure("format_error",QString("%1: %2").arg(context,client.error));
    }
};
template<class T> T field(TIFF *t, std::uint32_t tag) {
    T n{}; require(TIFFGetField(t,tag,&n) == 1,"missing required TIFF field"); return n;
}
}
void writeE(const QString &path, const Spec &s, const Source &source, FileIo &io, const Check &check, bool big) {
    const auto plan = storagePlan(s,true); check();
    TiffHandle handle(path,true,big,plan.ramBytes); auto t = handle.tif.get();
    auto set = [&](std::uint32_t tag, auto value) { handle.ok(TIFFSetField(t,tag,value) == 1,"TIFFSetField failed"); };
    set(TIFFTAG_IMAGEWIDTH,static_cast<std::uint32_t>(s.width));
    set(TIFFTAG_IMAGELENGTH,static_cast<std::uint32_t>(s.height));
    set(TIFFTAG_TILEWIDTH,s.side); set(TIFFTAG_TILELENGTH,s.side);
    set(TIFFTAG_SAMPLESPERPIXEL,1); set(TIFFTAG_BITSPERSAMPLE,s.wide ? 64 : 32);
    set(TIFFTAG_SAMPLEFORMAT,SAMPLEFORMAT_UINT); set(TIFFTAG_PLANARCONFIG,PLANARCONFIG_CONTIG);
    set(TIFFTAG_PHOTOMETRIC,PHOTOMETRIC_MINISBLACK); set(TIFFTAG_ORIENTATION,ORIENTATION_TOPLEFT);
    set(TIFFTAG_COMPRESSION,COMPRESSION_ADOBE_DEFLATE); set(TIFFTAG_ZIPQUALITY,6);
    set(TIFFTAG_PREDICTOR,PREDICTOR_NONE); set(TIFFTAG_DEFLATE_SUBCODEC,DEFLATE_SUBCODEC_ZLIB);
    for (U64 i = 0; i < logicalTiles(s); ++i) {
        check(); TileKey key{i%columns(s),i/columns(s)};
        auto tile = source(key); validateTile(s,tile,key);
        QByteArray raw(qsizetype(U64(s.side)*s.side*(s.wide ? 8 : 4)),0);
        for (U64 y = 0; y < tile.height; ++y) for (U64 x = 0; x < tile.width; ++x) {
            const U64 n = tile.counts[std::size_t(y*tile.width+x)];
            const auto pos = qsizetype((y*s.side+x)*(s.wide ? 8 : 4));
            if (s.wide) std::memcpy(raw.data()+pos,&n,8);
            else { auto narrow = static_cast<std::uint32_t>(n); std::memcpy(raw.data()+pos,&narrow,4); }
        }
        handle.ok(TIFFWriteEncodedTile(t,static_cast<std::uint32_t>(i),raw.data(),raw.size()) == raw.size(),"TIFF tile write failed");
    }
    check(); handle.ok(TIFFWriteDirectory(t) == 1,"TIFF directory finalization failed");
    handle.ok(TIFFFlush(t) == 1,"TIFF flush failed"); handle.tif.reset();
    handle.ok(true,"TIFF close failed"); handle.client.file.finish(); io = handle.client.file.io;
}
struct EReader::Impl {
    Spec spec;
    TiffHandle handle;
    U64 metadata = 0;
    Impl(const QString &path, const Spec &s, U64 budget) : spec(s), handle(path,false,true,budget) {
        storagePlan(s,true); auto t = handle.tif.get();
        require(TIFFIsTiled(t) && field<std::uint32_t>(t,TIFFTAG_IMAGEWIDTH) == s.width
            && field<std::uint32_t>(t,TIFFTAG_IMAGELENGTH) == s.height
            && field<std::uint32_t>(t,TIFFTAG_TILEWIDTH) == s.side && field<std::uint32_t>(t,TIFFTAG_TILELENGTH) == s.side
            && field<std::uint16_t>(t,TIFFTAG_BITSPERSAMPLE) == (s.wide ? 64 : 32)
            && field<std::uint16_t>(t,TIFFTAG_SAMPLESPERPIXEL) == 1
            && field<std::uint16_t>(t,TIFFTAG_SAMPLEFORMAT) == SAMPLEFORMAT_UINT
            && field<std::uint16_t>(t,TIFFTAG_PLANARCONFIG) == PLANARCONFIG_CONTIG
            && field<std::uint16_t>(t,TIFFTAG_COMPRESSION) == COMPRESSION_ADOBE_DEFLATE
            && field<std::uint16_t>(t,TIFFTAG_PREDICTOR) == PREDICTOR_NONE
            && TIFFNumberOfTiles(t) == logicalTiles(s),"unsupported or inconsistent TIFF metadata");
        std::uint64_t *offsets = nullptr, *lengths = nullptr;
        require(TIFFGetField(t,TIFFTAG_TILEOFFSETS,&offsets) && TIFFGetField(t,TIFFTAG_TILEBYTECOUNTS,&lengths),"missing TIFF tile arrays");
        U64 payload = 0, end = TIFFIsBigTIFF(t) ? 16 : 8;
        const auto size = U64(handle.client.file.file.size());
        for (U64 i = 0; i < logicalTiles(s); ++i) {
            require(offsets[i] >= end && offsets[i] <= size && lengths[i] && lengths[i] <= size-offsets[i]
                && lengths[i] <= compressBound(s.side*s.side*(s.wide ? 8UL : 4UL)),"missing/invalid TIFF tile offset or length");
            payload = add(payload,lengths[i]); end = add(offsets[i],lengths[i]);
        }
        metadata = size-payload; handle.ok(true,"TIFF metadata read failed");
        require(TIFFTileSize64(t) == U64(s.side)*s.side*(s.wide ? 8 : 4),"unexpected TIFF decode size");
    }
};
EReader::EReader(const QString &path, const Spec &s, U64 budget) {
    storagePlan(s,true); impl = std::make_unique<Impl>(path,s,budget);
}
EReader::~EReader() = default;
Tile EReader::read(TileKey key) {
    const auto &s = impl->spec; const auto ordinal = rasterOrdinal(s,key);
    auto tile = blank(s,key); const U64 bytes = U64(s.side)*s.side*(s.wide ? 8 : 4);
    QByteArray raw(qsizetype(bytes),Qt::Uninitialized);
    auto tif = impl->handle.tif.get();
    const auto length = TIFFGetStrileByteCount(tif,static_cast<std::uint32_t>(ordinal));
    require(length && length <= compressBound(static_cast<uLong>(bytes)),"invalid TIFF compressed length");
    QByteArray compressed(qsizetype(length),Qt::Uninitialized);
    impl->handle.ok(TIFFReadRawTile(tif,static_cast<std::uint32_t>(ordinal),compressed.data(),compressed.size())
        == compressed.size(),"TIFF raw tile read failed");
    // libtiff 4.7.2 ZIPDecode may stop when output is full without Z_STREAM_END.
    // This narrow Deflate/predictor=1 profile requires the complete zlib stream.
    uLongf output = static_cast<uLongf>(bytes); auto input = static_cast<uLong>(length);
    const auto code = uncompress2(reinterpret_cast<Bytef *>(raw.data()),&output,
        reinterpret_cast<const Bytef *>(compressed.constData()),&input);
    require(code == Z_OK && output == bytes && input == length,"incomplete/corrupt TIFF Deflate stream");
    const bool swapped = TIFFIsByteSwapped(tif);
    for (U64 y = 0; y < tile.height; ++y) for (U64 x = 0; x < tile.width; ++x) {
        const auto pos = qsizetype((y*s.side+x)*(s.wide ? 8 : 4)); U64 n = 0;
        if (s.wide) { std::memcpy(&n,raw.constData()+pos,8); if (swapped) n = qbswap(n); }
        else { std::uint32_t narrow; std::memcpy(&narrow,raw.constData()+pos,4);
            n = swapped ? qbswap(narrow) : narrow; }
        tile.counts[std::size_t(y*tile.width+x)] = n;
    }
    return tile;
}
std::vector<TileKey> EReader::physicalKeys() const {
    std::vector<TileKey> keys;
    const auto &s = impl->spec; keys.reserve(std::size_t(logicalTiles(s)));
    for (U64 i = 0; i < logicalTiles(s); ++i) keys.push_back({i%columns(s),i/columns(s)});
    return keys;
}
U64 EReader::metadataBytes() const { return impl->metadata; }
TrackedFile &EReader::tracked() { return impl->handle.client.file; }
} // namespace density::formats
