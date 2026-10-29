#include "density/density_storage.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QFile>
#include <zlib.h>
#define NOMINMAX
#include <Windows.h>
#include <Psapi.h>

using namespace cadcontour;
using namespace cadcontour::density;
namespace {
double ms(qint64 ns) { return double(ns)/1e6; }
QString number(U64 n) { return QString::number(n); }
PROCESS_MEMORY_COUNTERS memory()
{
    PROCESS_MEMORY_COUNTERS p{}; p.cb=sizeof(p); GetProcessMemoryInfo(GetCurrentProcess(),&p,sizeof(p)); return p;
}
void require(bool ok) { if(!ok) throw std::runtime_error("benchmark operation failed"); }
template<class T> T take(Result<T> result)
{
    if(!result) { const auto &e=result.error(); qCritical("phase=%d code=%d: %s",int(e.phase),int(e.code),qPrintable(e.detail)); }
    require(bool(result)); return std::move(result.value());
}
QJsonObject trial(const QString &parent, unsigned profile, bool large)
{
    constexpr U64 tiles=96; constexpr unsigned side=256;
    const U64 width=large?1'000'003:tiles*side, height=large?1'000'001:side;
    QTemporaryDir directory(parent+QStringLiteral("/.density062-XXXXXX")); require(directory.isValid());
    auto policy=initialResourcePolicy(); policy.sortBytes=160;
    QElapsedTimer timer; timer.start();
    auto m=raster::Mapping::create({0,0},{double(width),double(height)},1,policy.mapping); require(bool(m));
    const double inputMappingMs=ms(timer.nsecsElapsed());
    Metadata meta{DatasetId::create(),cache::CacheId::create(),0,BuildContract::SuppliedTiles,side,m.value()};
    auto before=memory(); timer.restart(); auto w=take(Writer::create(directory.filePath("map.dmap"),meta,policy,directory.path()));
    const double createMs=ms(timer.nsecsElapsed());
    std::vector<U64> counts(side*side); U64 state=6002;
    timer.restart();
    for(U64 i=tiles;i>0;--i) {
        for(std::size_t n=0;n<counts.size();++n) {
            state^=state<<13; state^=state>>7; state^=state<<17;
            counts[n]=profile==0?U64(1):profile==1?state&UINT32_MAX:U64(n==7||n==1234);
        }
        require(bool(w->addTile({i-1,0},{side,side},counts)));
    }
    const double ingestMs=ms(timer.nsecsElapsed()); timer.restart(); auto pending=take(w->finish());
    const double finishMs=ms(timer.nsecsElapsed()); const auto ws=w->statistics();
    timer.restart(); auto r=take(Reader::openReader(pending.path(),policy)); const double openMs=ms(timer.nsecsElapsed());
    const auto openStats=r->statistics(); timer.restart();
    for(U64 key:{U64(0),U64(17),U64(37),U64(71),U64(95)}) { auto t=take(r->readTile({key,0})); require(t.counts.size()==side*side); }
    const double selectiveMs=ms(timer.nsecsElapsed()); const auto selective=r->statistics();
    struct Consumer { U64 tiles=0, digest=0; } consumer;
    auto sink=[](const Tile &t,void *p) noexcept -> Status { auto &c=*static_cast<Consumer *>(p); ++c.tiles;
        for(auto n:t.counts)c.digest=(c.digest*1099511628211ULL)^n; return success(); };
    timer.restart(); require(bool(r->forEachTile(+sink,&consumer))); const double fullMs=ms(timer.nsecsElapsed());
    require(consumer.tiles==tiles); const auto after=memory();
    return {{"profile",profile==0?"compressible":profile==1?"entropy":"sparse"},{"large_area",large},
        {"width",number(width)},{"height",number(height)},{"side",int(side)},{"T",number(tiles)},
        {"file_bytes",number(ws.outputBytes)},{"index_bytes",number(tiles*80)},{"scratch_peak_logical_bytes",number(ws.scratchPeak)},
        {"combined_disk_peak_logical_bytes",number(ws.combinedDiskPeak)},{"buffer_reserve_bytes",number(ws.bufferReserve)},
        {"codec_peak_charged_bytes",number(ws.codecPeak)},{"sort_reserve_bytes",number(ws.sortReserve)},{"merge_passes",number(ws.mergePasses)},
        {"input_mapping_ms",inputMappingMs},{"writer_create_ms",createMs},{"ingest_ms",ingestMs},{"finish_ms",finishMs},
        {"sort_merge_ms",ms(ws.sortNanoseconds)},{"payload_index_copy_ms",ms(ws.copyNanoseconds)},{"readback_ms",ms(ws.readbackNanoseconds)},
        {"writer_mapping_total_ms",ms(ws.mappingNanoseconds)},{"metadata_open_ms",openMs},{"reader_mapping_ms",ms(openStats.mappingNanoseconds)},
        {"selective_5_ms",selectiveMs},{"full_physical_iteration_ms",fullMs},
        {"open_read_bytes",number(openStats.readBytes)},{"open_decoded_tiles",number(openStats.decodedTiles)},
        {"selective_read_bytes",number(selective.readBytes-openStats.readBytes)},{"selective_decoded_tiles",number(selective.decodedTiles)},
        {"full_read_bytes",number(r->statistics().readBytes-selective.readBytes)},
        {"writer_logical_read_bytes",number(ws.readBytes)},{"writer_logical_written_bytes",number(ws.writtenBytes)},
        {"working_set_before",number(before.WorkingSetSize)},{"working_set_after",number(after.WorkingSetSize)},
        {"process_lifetime_peak_working_set",number(after.PeakWorkingSetSize)},{"digest",number(consumer.digest)}};
}
}
int main(int argc,char **argv)
{
    QCoreApplication app(argc,argv);
    if(app.arguments().size()!=3) { qInfo("Usage: density_storage_benchmark <existing scratch parent> <output.json>"); return 2; }
    try {
        QJsonArray rows; for(unsigned profile=0;profile<3;++profile)for(bool large:{false,true})rows.append(trial(app.arguments()[1],profile,large));
        QJsonObject root{{"schema","density-storage-06.2-v1"},{"qt",QT_VERSION_STR},{"zlib",zlibVersion()},
            {"msvc",_MSC_FULL_VER},{"rows",rows},{"note","Synthetic counts; logical I/O and sampled working sets; OS peak is process lifetime, warm cache, no power-loss/cold-HDD claim."}};
        QFile out(app.arguments()[2]); require(out.open(QIODevice::WriteOnly|QIODevice::Truncate));
        const auto data=QJsonDocument(root).toJson(); require(out.write(data)==data.size()); return 0;
    } catch(const std::exception &e) { qCritical("%s",e.what()); return 1; }
}
