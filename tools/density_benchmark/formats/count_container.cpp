#include "formats.h"
#include <QtEndian>
#include <zlib.h>
#include <algorithm>
#include <cstring>

namespace density::formats {
namespace {
constexpr U64 headerBytes = 96, entryBytes = 40;
template<class T> T get(const QByteArray &b, qsizetype offset) { return qFromLittleEndian<T>(b.constData()+offset); }
template<class T> void put(QByteArray &b, qsizetype offset, T n) { qToLittleEndian<T>(n,b.data()+offset); }
std::uint32_t crc(const QByteArray &b) {
    return static_cast<std::uint32_t>(crc32_z(0,reinterpret_cast<const Bytef *>(b.constData()),static_cast<z_size_t>(b.size())));
}
}
void writeD(const QString &path, const Spec &s, const Source &source, FileIo &io, const Check &check) {
    storagePlan(s,false); check(); TrackedFile f(path,true);
    QByteArray h(headerBytes,0); f.write(h);
    QByteArray index;
    for (U64 i = 0; i < tileCount(s); ++i) {
        check(); const auto key = keyAt(s,i); auto t = source(key); validateTile(s,t,key);
        if (std::all_of(t.counts.begin(),t.counts.end(),[](U64 n) { return n == 0; })) continue;
        QByteArray raw(static_cast<qsizetype>(t.counts.size()*(s.wide ? 8 : 4)),Qt::Uninitialized);
        for (std::size_t n = 0; n < t.counts.size(); ++n) {
            if (s.wide) put(raw,qsizetype(n*8),t.counts[n]);
            else put(raw,qsizetype(n*4),static_cast<std::uint32_t>(t.counts[n]));
        }
        uLongf length = compressBound(static_cast<uLong>(raw.size()));
        QByteArray compressed(length,Qt::Uninitialized);
        if (compress2(reinterpret_cast<Bytef *>(compressed.data()),&length,
            reinterpret_cast<const Bytef *>(raw.constData()),static_cast<uLong>(raw.size()),6) != Z_OK)
            throw Failure("codec_error","Deflate compression failed");
        compressed.resize(length);
        QByteArray entry(entryBytes,0); put(entry,0,rasterOrdinal(s,key)); put(entry,8,U64(f.file.pos()));
        put(entry,16,U64(length)); put(entry,24,U64(raw.size())); put(entry,32,crc(raw));
        index.append(entry); f.write(compressed);
    }
    const auto indexOffset = U64(f.file.pos()); f.write(index);
    std::memcpy(h.data(),"D06BCT01",8); put(h,8,std::uint32_t(1)); put(h,12,std::uint32_t(s.wide ? 64 : 32));
    put(h,16,s.width); put(h,24,s.height); put(h,32,s.side); put(h,40,U64(index.size())/entryBytes);
    put(h,48,indexOffset); put(h,56,U64(f.file.pos())); put(h,68,crc(index)); put(h,64,crc(h));
    check(); f.seek(0); f.write(h); f.finish(); io = f.io;
}
DReader::DReader(const QString &path, const Spec &s, U64 budget) : file(path,false), spec(s) {
    storagePlan(s,false);
    auto h = file.read(0,headerBytes);
    const auto headerCrc = get<std::uint32_t>(h,64); put(h,64,std::uint32_t(0));
    require(h.first(8) == "D06BCT01" && get<std::uint32_t>(h,8) == 1 && crc(h) == headerCrc,
        "invalid count container header/version/checksum");
    require(get<std::uint32_t>(h,12) == (s.wide ? 64U : 32U) && get<U64>(h,16) == s.width
        && get<U64>(h,24) == s.height && get<std::uint32_t>(h,32) == s.side
        && get<std::uint32_t>(h,36) == 0 && h.mid(72) == QByteArray(24,0), "count metadata mismatch");
    const auto n = get<U64>(h,40), offset = get<U64>(h,48), size = get<U64>(h,56);
    require(size == U64(file.file.size()) && offset >= headerBytes && offset <= size
        && n <= logicalTiles(s) && multiply(n,entryBytes) == size-offset, "invalid count index bounds");
    if (multiply(n,entryBytes+sizeof(Entry)) > budget) throw Failure("resource_refused","count index exceeds budget");
    auto index = file.read(offset,multiply(n,entryBytes));
    require(crc(index) == get<std::uint32_t>(h,68),"invalid count index checksum");
    entries.reserve(static_cast<std::size_t>(n));
    U64 end = headerBytes;
    for (U64 i = 0; i < n; ++i) {
        const auto p = qsizetype(i*entryBytes);
        Entry e{get<U64>(index,p),get<U64>(index,p+8),get<U64>(index,p+16),get<U64>(index,p+24),get<std::uint32_t>(index,p+32)};
        require(e.ordinal < logicalTiles(s) && (entries.empty() || e.ordinal > entries.back().ordinal)
            && e.offset == end && e.offset <= offset && e.length <= offset-e.offset
            && get<std::uint32_t>(index,p+36) == 0, "invalid count index entry/order");
        auto k = TileKey{e.ordinal%columns(s),e.ordinal/columns(s)};
        const auto expected = std::min<U64>(s.side,s.width-k.x*s.side)*std::min<U64>(s.side,s.height-k.y*s.side)*(s.wide ? 8 : 4);
        require(e.raw == expected && e.length && e.length <= compressBound(static_cast<uLong>(expected)), "invalid compressed tile size");
        end = add(e.offset,e.length); entries.push_back(e);
    }
    require(end == offset,"unindexed count payload");
}
Tile DReader::read(TileKey key) {
    const auto ordinal = rasterOrdinal(spec,key);
    const auto it = std::lower_bound(entries.begin(),entries.end(),ordinal,[](const Entry &e,U64 n) { return e.ordinal < n; });
    auto tile = blank(spec,key);
    if (it == entries.end() || it->ordinal != ordinal) return tile;
    auto b = file.read(it->offset,it->length);
    QByteArray raw(static_cast<qsizetype>(it->raw),Qt::Uninitialized);
    uLongf bytes = static_cast<uLongf>(raw.size()); auto input = static_cast<uLong>(b.size());
    const auto code = uncompress2(reinterpret_cast<Bytef *>(raw.data()),&bytes,
        reinterpret_cast<const Bytef *>(b.constData()),&input);
    require(code == Z_OK && bytes == it->raw && input == it->length && crc(raw) == it->crc, "corrupt count tile payload");
    for (std::size_t i = 0; i < tile.counts.size(); ++i)
        tile.counts[i] = spec.wide ? get<U64>(raw,qsizetype(i*8)) : get<std::uint32_t>(raw,qsizetype(i*4));
    return tile;
}
std::vector<TileKey> DReader::physicalKeys() const {
    std::vector<TileKey> keys; keys.reserve(entries.size());
    for (const auto &e : entries) keys.push_back({e.ordinal%columns(spec),e.ordinal/columns(spec)});
    return keys;
}
U64 DReader::metadataBytes() const { return headerBytes+entries.size()*entryBytes; }
} // namespace density::formats
