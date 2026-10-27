#include "construction.h"
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <set>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>

namespace density::construction {
namespace cache = cadcontour::cache;
namespace {
double elapsed(const QElapsedTimer &t) { return double(t.nsecsElapsed()) / 1000000.; }
double boundary(double origin, double step, U64 i) { return origin + double(i) * step; }
U64 axis(double lo, double hi, double step, U64 n, double point) {
    require(std::isfinite(point) && point >= lo && point <= hi, "point outside raster");
    if (n == 1) return 0;
    U64 first = 0, last = n;
    while (first + 1 < last) {
        const U64 mid = first + (last - first) / 2;
        if (point < boundary(lo, step, mid)) last = mid; else first = mid;
    }
    return first;
}
void write(QFile &f, QByteArrayView bytes, FileIo &io) {
    qsizetype pos = 0;
    while (pos < bytes.size()) {
        const auto n = f.write(bytes.data() + pos, bytes.size() - pos); ++io.writes;
        if (n <= 0) throw Failure("io_error", f.errorString());
        io.writeBytes = add(io.writeBytes, U64(n)); pos += n;
    }
}
QByteArray read(QFile &f, U64 size, FileIo &io) {
    require(size <= U64(std::numeric_limits<qsizetype>::max()), "read size overflow");
    QByteArray bytes(qsizetype(size), Qt::Uninitialized); qsizetype pos = 0;
    while (pos < bytes.size()) {
        const auto n = f.read(bytes.data() + pos, bytes.size() - pos); ++io.reads;
        if (n <= 0) throw Failure("io_error", "short count/event read: " + f.errorString());
        io.readBytes = add(io.readBytes, U64(n)); pos += n;
    }
    return bytes;
}
void accumulate(cache::ReadStatistics &a, const cache::ReadStatistics &b) {
    a.indexPages = add(a.indexPages,b.indexPages); a.pointPages = add(a.pointPages,b.pointPages);
    a.readBytes = add(a.readBytes,b.readBytes); a.readOperations = add(a.readOperations,b.readOperations);
    a.seekOperations = add(a.seekOperations,b.seekOperations); a.returnedRecords = add(a.returnedRecords,b.returnedRecords);
    a.bufferBytes = std::max(a.bufferBytes,b.bufferBytes); a.traversalFrames = std::max(a.traversalFrames,b.traversalFrames);
}
template<class Operation> cache::ReadStatistics scan(Operation operation, const std::function<void(std::span<const cache::PointRecord>)> &sink) {
    // CacheReader intentionally translates thrown consumers. Preserve the original
    // experiment failure (including cancellation / resource refusal / I/O error).
    std::exception_ptr failure;
    auto result = operation([&](std::span<const cache::PointRecord> records) -> cache::Status {
        try { sink(records); return cache::success(); }
        catch (...) { failure = std::current_exception(); return cache::makeError(cache::ErrorCode::ConsumerFailed,
            cache::Phase::CacheRead, cache::Operation::Consume, "experiment consumer failed"); }
    });
    if (failure) std::rethrow_exception(failure);
    return take(std::move(result));
}
bool lessKey(TileKey a, TileKey b) { return a.y < b.y || (a.y == b.y && a.x < b.x); }
U64 allocatedBytes(QFile &file) {
    const int descriptor = file.handle();
    if (descriptor < 0) throw Failure("io_error","owned spool descriptor unavailable");
    const auto handle = reinterpret_cast<HANDLE>(_get_osfhandle(descriptor));
    FILE_STANDARD_INFO info{};
    if (handle == INVALID_HANDLE_VALUE || !GetFileInformationByHandleEx(handle,FileStandardInfo,&info,sizeof(info)) || info.AllocationSize.QuadPart < 0)
        throw Failure("io_error","cannot query owned spool allocation");
    return U64(info.AllocationSize.QuadPart);
}
}
Layout layout(const Parameters &p) {
    require(p.tileSide && p.tileSide <= 1024 && p.groupTiles && p.eventCapacity && p.maxGroups && p.maxOccupiedGroups, "invalid construction parameters");
    require(multiply(p.eventCapacity,24) <= U64(std::numeric_limits<qsizetype>::max()),"event chunk size unrepresentable");
    auto expected = makeGrid(p.grid.bounds,p.grid.cell);
    require(toJson(expected) == toJson(p.grid), "inconsistent grid dimensions");
    require(p.grid.width <= (1ULL << 53) && p.grid.height <= (1ULL << 53), "grid indices not exactly binary64 representable");
    if (add(p.grid.width-1,p.grid.height-1) > p.maxBoundaryChecks)
        throw Failure("resource_refused","experimental boundary validation work limit exceeded");
    const U64 side = multiply(p.tileSide,p.groupTiles), cells = multiply(side,side);
    require(cells <= std::numeric_limits<std::uint32_t>::max(), "local event index exceeds uint32");
    const U64 columns = (p.grid.width-1)/side+1, rows = (p.grid.height-1)/side+1, groups = multiply(columns,rows);
    return {side,columns,rows,groups,cells,{"raster-counts",p.grid.width,p.grid.height,p.tileSide,0,true}};
}
Plan plan(const Parameters &p, U64 points) {
    auto l = layout(p);
    const auto tiles = multiply((p.grid.width-1)/p.tileSide+1,(p.grid.height-1)/p.tileSide+1);
    const auto occupied = std::min({points,l.groups,p.maxOccupiedGroups});
    const auto nonzero = std::min({tiles,points,multiply(occupied,multiply(p.groupTiles,p.groupTiles))});
    // The same conservative envelope is used by both candidates. Includes runtime,
    // reader, group counts, tile/codec copies, event+encoded buffers, descriptors,
    // and occupied spool IDs. Disk accounts logical EOF plus per-file reserve.
    return {add(48*MiB,add(multiply(l.cells,8),add(multiply(U64(p.tileSide)*p.tileSide,32),
            add(multiply(p.eventCapacity,24),add(multiply(nonzero,64),multiply(occupied,80)))))),
        add(multiply(points,4),multiply(occupied,4096)),
        add(2*MiB,multiply(nonzero,add(32,multiply(U64(p.tileSide)*p.tileSide,8))))};
}
TileKey cell(const Grid &g, cadcontour::ModelPoint p) {
    return {axis(g.bounds.minX,g.bounds.maxX,g.cell,g.width,p.x),axis(g.bounds.minY,g.bounds.maxY,g.cell,g.height,p.y)};
}
void validateGrid(const Grid &g, const Check &check) {
    auto validate = [&](double lo, double hi, U64 n) {
        double previous = lo;
        for (U64 i = 1; i < n; ++i) {
            if (!(i % 4096)) check();
            const double b = boundary(lo,g.cell,i);
            require(std::isfinite(b) && b > previous && b <= hi, "collapsed/invalid binary64 raster interval");
            previous = b;
        }
    };
    validate(g.bounds.minX,g.bounds.maxX,g.width); validate(g.bounds.minY,g.bounds.maxY,g.height);
}
cache::BoundingBox queryBox(const Parameters &p, U64 group) {
    const auto l = layout(p); require(group < l.groups,"group outside raster");
    const U64 x0 = (group%l.columns)*l.side, y0 = (group/l.columns)*l.side;
    const U64 x1 = std::min(p.grid.width,add(x0,l.side)), y1 = std::min(p.grid.height,add(y0,l.side));
    // Bounds use exactly the mapping's rounded binary64 expressions, not inverse
    // division. Inclusive upper bounds can return neighbours; ownership filters
    // them. One representable step outward is conservative, with metadata clipping.
    auto lower = [&](double origin, U64 i) {
        return std::max(origin,std::nextafter(boundary(origin,p.grid.cell,i),-std::numeric_limits<double>::infinity()));
    };
    auto upper = [&](double origin, double maximum, U64 i, U64 n) {
        return i == n ? maximum : std::min(maximum,std::nextafter(boundary(origin,p.grid.cell,i),std::numeric_limits<double>::infinity()));
    };
    return {lower(p.grid.bounds.minX,x0),lower(p.grid.bounds.minY,y0),
        upper(p.grid.bounds.minX,p.grid.bounds.maxX,x1,p.grid.width),upper(p.grid.bounds.minY,p.grid.bounds.maxY,y1,p.grid.height)};
}
QJsonObject build(Candidate candidate, cache::CacheReader &reader, const Parameters &p,
                  const QString &scratch, const TileSink &sink, FileIo &io, const Check &check) {
    const auto l = layout(p);
    if (candidate == Candidate::Spatial && l.groups > p.maxGroups)
        throw Failure("resource_refused","experimental spatial maxGroups exceeded");
    preflight(QFileInfo(scratch).absolutePath(),p.limits,plan(p,reader.metadata().pointCount));
    check(); QElapsedTimer timer; timer.start(); validateGrid(p.grid,check); const double validationMs = elapsed(timer);
    const auto b = reader.metadata().bounds, g = p.grid.bounds;
    require(!reader.metadata().pointCount || (b.minX >= g.minX && b.minY >= g.minY && b.maxX <= g.maxX && b.maxY <= g.maxY),"grid does not cover cache");
    require(!QFileInfo::exists(scratch) && QDir().mkdir(scratch),"scratch path must be new");
    QTemporaryDir owned(QDir(scratch).filePath("events-XXXXXX"));
    if (!owned.isValid()) throw Failure("io_error","cannot create event scratch");
    std::vector<U64> counts(std::size_t(l.cells),0);
    struct Event { U64 group; std::uint32_t local; };
    std::vector<Event> events;
    std::set<U64> occupied;
    U64 points = 0, queries = 0, opens = 0, scratchBytes = 0, scratchPeak = 0, rejected = 0;
    U64 visited = 0, occupiedCount = 0, allocated = 0, allocatedPeak = 0;
    FileIo spoolIo;
    double scanMs = 0, callbackMs = 0, sortMs = 0, writeMs = 0, readMs = 0, countMs = 0, outputMs = 0;
    double traversalMs = 0, spoolLifecycleMs = 0;
    cache::ReadStatistics stats;
    auto eventPath = [&](U64 group) { return QDir(owned.path()).filePath(decimal(group)+".events"); };
    auto flush = [&] {
        if (events.empty()) return;
        check(); QElapsedTimer t; t.start();
        std::sort(events.begin(),events.end(),[](const Event &a,const Event &b) { return a.group < b.group; });
        sortMs += elapsed(t); t.restart();
        for (std::size_t first = 0; first < events.size();) {
            check(); auto last = first+1; while (last < events.size() && events[last].group == events[first].group) ++last;
            const auto group = events[first].group;
            const bool existing = occupied.contains(group);
            if (!existing && occupied.size() == p.maxOccupiedGroups)
                throw Failure("resource_refused","experimental maxOccupiedGroups exceeded");
            QFile f(eventPath(group)); ++opens;
            if (!f.open(QIODevice::WriteOnly | QIODevice::Unbuffered | (existing ? QIODevice::Append : QIODevice::NewOnly)))
                throw Failure("io_error",f.errorString());
            const U64 allocationBefore = existing ? allocatedBytes(f) : 0;
            QByteArray encoded(qsizetype((last-first)*4),Qt::Uninitialized);
            for (auto i = first; i < last; ++i) qToLittleEndian<std::uint32_t>(events[i].local,encoded.data()+(i-first)*4);
            const auto before = io; write(f,encoded,io);
            spoolIo.writeBytes = add(spoolIo.writeBytes,io.writeBytes-before.writeBytes); spoolIo.writes = add(spoolIo.writes,io.writes-before.writes);
            if (!f.flush()) throw Failure("io_error",f.errorString());
            allocated = add(allocated-allocationBefore,allocatedBytes(f));
            allocatedPeak = std::max(allocatedPeak,allocated);
            if (allocated > p.limits.temporaryBytes) throw Failure("resource_refused","allocated event scratch budget exceeded");
            scratchBytes = add(scratchBytes,U64(encoded.size())); scratchPeak = std::max(scratchPeak,scratchBytes);
            if (scratchBytes > p.limits.temporaryBytes) throw Failure("resource_refused","event scratch budget exceeded");
            occupied.insert(group); first = last;
        }
        events.clear(); writeMs += elapsed(t);
    };
    auto address = [&](const cache::PointRecord &r) {
        auto c = cell(p.grid,r.point);
        return Event{(c.y/l.side)*l.columns+c.x/l.side,std::uint32_t((c.y%l.side)*l.side+c.x%l.side)};
    };
    if (candidate == Candidate::Sequential) {
        events.reserve(std::size_t(p.eventCapacity)); timer.restart();
        accumulate(stats,scan([&](const auto &consumer) { return reader.scanAll(consumer); },[&](auto batch) {
            QElapsedTimer t; t.start(); check();
            for (const auto &r : batch) { events.push_back(address(r)); if (events.size() == p.eventCapacity) flush(); }
            callbackMs += elapsed(t);
        }));
        scanMs += elapsed(timer); flush();
    }
    auto occupiedIt = occupied.cbegin();
    U64 spatialGroup = 0;
    QElapsedTimer traversal; traversal.start();
    while (candidate == Candidate::Sequential ? occupiedIt != occupied.cend() : spatialGroup < l.groups) {
        check();
        const U64 group = candidate == Candidate::Sequential ? *occupiedIt++ : spatialGroup++;
        ++visited; traversalMs += elapsed(traversal);
        timer.restart(); std::fill(counts.begin(),counts.end(),0); countMs += elapsed(timer);
        if (candidate == Candidate::Sequential) {
            ++occupiedCount; QElapsedTimer lifecycle; lifecycle.start();
            QFile f(eventPath(group)); ++opens;
            if (!f.open(QIODevice::ReadOnly | QIODevice::Unbuffered)) throw Failure("io_error",f.errorString());
            const U64 bytes = U64(f.size()); require(bytes%4 == 0,"invalid event spool size");
            const U64 allocation = allocatedBytes(f); spoolLifecycleMs += elapsed(lifecycle);
            for (U64 pos = 0; pos < bytes;) {
                check(); timer.restart(); const auto before = io;
                auto encoded = read(f,std::min<U64>(bytes-pos,p.eventCapacity*4),io); readMs += elapsed(timer);
                spoolIo.readBytes = add(spoolIo.readBytes,io.readBytes-before.readBytes); spoolIo.reads = add(spoolIo.reads,io.reads-before.reads);
                timer.restart();
                for (qsizetype i = 0; i < encoded.size(); i += 4) {
                    const auto local = qFromLittleEndian<std::uint32_t>(encoded.data()+i);
                    require(local < l.cells,"local event index outside group");
                    counts[local] = add(counts[local],1); points = add(points,1);
                }
                countMs += elapsed(timer); pos += U64(encoded.size());
            }
            lifecycle.restart(); f.close(); if (!QFile::remove(f.fileName())) throw Failure("io_error","cannot remove owned event spool");
            spoolLifecycleMs += elapsed(lifecycle); allocated -= allocation;
            scratchBytes -= bytes;
        } else {
            const U64 beforePoints = points;
            timer.restart(); const auto box = queryBox(p,group); ++queries;
            accumulate(stats,scan([&](const auto &consumer) { return reader.scanBox(box,consumer); },[&](auto batch) {
                QElapsedTimer t; t.start(); check();
                for (const auto &r : batch) {
                    const auto a = address(r);
                    if (a.group != group) { ++rejected; continue; }
                    counts[a.local] = add(counts[a.local],1); points = add(points,1);
                }
                const auto duration = elapsed(t); callbackMs += duration; countMs += duration;
            }));
            scanMs += elapsed(timer);
            if (points != beforePoints && ++occupiedCount > p.maxOccupiedGroups)
                throw Failure("resource_refused","experimental maxOccupiedGroups exceeded");
        }
        const U64 gx = (group%l.columns)*l.side, gy = (group/l.columns)*l.side;
        for (U64 ty = gy; ty < std::min(p.grid.height,gy+l.side); ty += p.tileSide) {
            for (U64 tx = gx; tx < std::min(p.grid.width,gx+l.side); tx += p.tileSide) {
                check(); timer.restart();
                Tile tile{{tx/p.tileSide,ty/p.tileSide},std::uint32_t(std::min<U64>(p.tileSide,p.grid.width-tx)),
                    std::uint32_t(std::min<U64>(p.tileSide,p.grid.height-ty)),{}};
                tile.counts.reserve(std::size_t(tile.width)*tile.height); U64 total = 0;
                for (U64 y = 0; y < tile.height; ++y) for (U64 x = 0; x < tile.width; ++x) {
                    auto n = counts[(ty-gy+y)*l.side+tx-gx+x]; tile.counts.push_back(n); total = add(total,n);
                }
                countMs += elapsed(timer);
                if (total) { timer.restart(); sink(tile); outputMs += elapsed(timer); }
            }
        }
        traversal.restart();
    }
    check(); require(points == reader.metadata().pointCount,"raster sum differs from cache measurement count");
    return {{"grid",toJson(p.grid)},{"tileSide",int(p.tileSide)},{"groupTiles",int(p.groupTiles)},
        {"groups",decimal(l.groups)},{"seenPoints",decimal(points)},{"queries",decimal(queries)},
        {"visitedGroups",decimal(visited)},{"occupiedGroups",decimal(occupiedCount)},{"groupTraversalMs",traversalMs},
        {"cacheReaderScans",cacheStatistics(stats)},{"scanInclusiveMs",scanMs},{"scanExcludingCallbacksMs",std::max(0.,scanMs-callbackMs)},
        {"callbackInclusiveMs",callbackMs},{"boundaryValidationMs",validationMs},{"eventGroupingMs",sortMs},
        {"spoolWriteMs",writeMs},{"spoolReadMs",readMs},{"spoolIo",ioJson(spoolIo)},{"countAndTileMs",countMs},{"outputSinkMs",outputMs},
        {"scratchLogicalPeakBytes",decimal(scratchPeak)},{"scratchFinalBytes",decimal(scratchBytes)},
        {"scratchAllocatedPeakBytes",decimal(allocatedPeak)},{"scratchAllocatedFinalBytes",decimal(allocated)},
        {"spoolOpenRemoveMs",spoolLifecycleMs},{"occupiedMetadataPlannedBytes",decimal(std::min({reader.metadata().pointCount,l.groups,p.maxOccupiedGroups})*80)},
        {"scratchPeakDefinition","exact sum of owned event EOF lengths; monotone write phase then remove; excludes filesystem allocation"},
        {"spoolFileOpens",decimal(opens)},{"spoolFiles",decimal(U64(occupied.size()))},
        {"peakOwnedDatasetFileHandles",candidate == Candidate::Sequential && !occupied.empty() ? 3 : 2},
        {"groupBufferBytes",decimal(l.cells*8)},{"eventBufferBytes",decimal(U64(events.capacity())*sizeof(Event))},
        {"boundaryNeighbourRecordsRejected",decimal(rejected)}};
}
Output::Output(const QString &path, const Spec &spec, U64 points, U64 budget, FileIo &io, U64 maxTiles)
    : file_(path),spec_(spec),budget_(budget),io_(io) {
    require(!QFileInfo::exists(path),"output exists");
    if (!file_.open(QIODevice::ReadWrite | QIODevice::NewOnly | QIODevice::Unbuffered)) throw Failure("io_error",file_.errorString());
    const U64 tiles = multiply((spec.width-1)/spec.side+1,(spec.height-1)/spec.side+1);
    entries_.reserve(std::size_t(std::min({tiles,points,maxTiles})));
}
void Output::append(const Tile &tile) {
    require(!finished_,"output already finished"); QElapsedTimer t; t.start();
    auto encoded = encode(tile); const auto next = add(bytes_,U64(encoded.size()));
    if (next > budget_ || entries_.size() == entries_.capacity()) throw Failure("resource_refused","count output budget exceeded");
    write(file_,encoded,io_); entries_.push_back({tile.key,bytes_,U64(encoded.size())});
    bytes_ = next; total_ = add(total_,sum(tile)); outputMs += elapsed(t);
}
void Output::finish(U64 expected) {
    QElapsedTimer t; t.start(); require(total_ == expected,"output sum mismatch");
    std::sort(entries_.begin(),entries_.end(),[](const Entry &a,const Entry &b) { return lessKey(a.key,b.key); });
    for (std::size_t i = 1; i < entries_.size(); ++i) require(lessKey(entries_[i-1].key,entries_[i].key),"duplicate output tile");
    if (!file_.flush()) throw Failure("io_error",file_.errorString());
    finished_ = true; outputMs += elapsed(t);
}
QJsonObject Output::verify(const std::vector<U64> &expected, const Check &check) {
    require(finished_,"partial output cannot be verified as complete");
    QCryptographicHash hash(QCryptographicHash::Sha256); hash.addData(QJsonDocument(toJson(spec_)).toJson(QJsonDocument::Compact));
    U64 total = 0, maximum = 0, nonzero = 0; FileIo validationIo;
    std::vector<U64> actual;
    if (!expected.empty()) { require(expected.size() == spec_.width*spec_.height,"invalid expected grid"); actual.resize(expected.size()); }
    for (const auto &e : entries_) {
        check(); if (!file_.seek(qint64(e.offset))) throw Failure("io_error",file_.errorString());
        auto encoded = read(file_,e.bytes,validationIo); auto tile = decode(encoded);
        require(tile.key == e.key && tile.width == std::min<U64>(spec_.side,spec_.width-e.key.x*spec_.side)
            && tile.height == std::min<U64>(spec_.side,spec_.height-e.key.y*spec_.side),"output tile address/shape mismatch");
        require(sum(tile) != 0,"explicit zero tile in sparse output"); hash.addData(encoded); total = add(total,sum(tile));
        for (U64 y = 0; y < tile.height; ++y) for (U64 x = 0; x < tile.width; ++x) {
            auto n = tile.counts[y*tile.width+x]; maximum = std::max(maximum,n); nonzero += n != 0;
            if (!actual.empty()) actual[(e.key.y*spec_.side+y)*spec_.width+e.key.x*spec_.side+x] = n;
        }
    }
    require(total == total_,"readback sum mismatch"); require(expected.empty() || expected == actual,"independent oracle mismatch");
    return {{"sha256",QString::fromLatin1(hash.result().toHex())},{"sum",decimal(total)},{"maximum",decimal(maximum)},
        {"nonzeroCells",decimal(nonzero)},{"storedTiles",decimal(U64(entries_.size()))},{"outputBytes",decimal(bytes_)},
        {"oracleAvailable",!expected.empty()},{"digestDefinition","SHA256 Spec + nonzero encode(Tile), sorted Y/X; zero tiles implicit"}};
}
} // namespace density::construction
