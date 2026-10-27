#include "construction.h"
#include "cache/cache_writer.h"
#include <QDir>
#include <QJsonDocument>
#include <QProcess>
#include <QTemporaryDir>
#include <gtest/gtest.h>
#include <cmath>
#include <random>

using namespace density;
using namespace density::construction;
namespace cache = cadcontour::cache;
namespace {
auto noop = [] {};
QString writeCache(QTemporaryDir &root, const std::vector<cache::Measurement> &points, const QString &name = "input") {
    auto path = root.filePath(name+".pc2dcache");
    auto writer = take(cache::CacheWriter::create(path,{8*MiB,64*MiB,4},root.path()));
    take(writer->append(points)); auto pending = take(writer->finish()); pending.releaseToOwner(); return path;
}
std::vector<U64> run(const QString &path,QTemporaryDir &root,Parameters p,Candidate candidate,QString suffix = {}) {
    auto reader = take(cache::CacheReader::open(path,{8*MiB,0,1})); FileIo io;
    std::vector<U64> result(std::size_t(p.grid.width*p.grid.height));
    build(candidate,*reader,p,root.filePath("scratch"+suffix),[&](const Tile &tile) {
        for (U64 y = 0; y < tile.height; ++y) for (U64 x = 0; x < tile.width; ++x) {
            auto &n = result[(tile.key.y*p.tileSide+y)*p.grid.width+tile.key.x*p.tileSide+x];
            EXPECT_EQ(n,0); n = tile.counts[y*tile.width+x];
        }
    },io,noop); return result;
}
TEST(DensityConstruction, ManualLabelsBothCandidatesAndPartialDegenerateAxes) {
    for (const auto &c : rasterCases()) {
        QTemporaryDir root; auto path = writeCache(root,c.points);
        const auto expected = oracleCounts(c.grid,c.points);
        for (auto tile : {1U,2U,4U}) for (auto group : {1U,2U,3U}) for (auto candidate : {Candidate::Sequential,Candidate::Spatial}) {
            Parameters p; p.grid = c.grid; p.tileSide = tile; p.groupTiles = group; p.eventCapacity = 3;
            EXPECT_EQ(run(path,root,p,candidate,QString("-%1-%2-%3").arg(tile).arg(group).arg(int(candidate))),expected) << c.name.toStdString();
        }
    }
}
TEST(DensityConstruction, NextafterNonRepresentableCellAndGroupBoundaries) {
    for (double step : {.1,.3,.5,1.,2.5}) {
        QTemporaryDir root; Grid grid = makeGrid({-3.125,-2.75,3.375,2.25},step);
        std::vector<cache::Measurement> points;
        for (U64 i = 1; i < grid.width; ++i) {
            const double b = grid.bounds.minX+double(i)*step;
            for (double x : {std::nextafter(b,-INFINITY),b,std::nextafter(b,INFINITY)}) {
                if (x <= grid.bounds.maxX) points.push_back({x,grid.bounds.minY,0});
            }
        }
        for (U64 i = 1; i < grid.height; ++i) {
            const double b = grid.bounds.minY+double(i)*step;
            for (double y : {std::nextafter(b,-INFINITY),b,std::nextafter(b,INFINITY)}) if (y <= grid.bounds.maxY) points.push_back({0,y,0});
        }
        points.push_back({grid.bounds.maxX,grid.bounds.maxY,0});
        auto path = writeCache(root,points); auto expected = oracleCounts(grid,points);
        Parameters p; p.grid = grid; p.tileSide = 2; p.groupTiles = 2;
        for (auto candidate : {Candidate::Sequential,Candidate::Spatial})
            EXPECT_EQ(run(path,root,p,candidate,QString::number(int(candidate))),expected);
    }
}
TEST(DensityConstruction, PassesDuplicatesDenseSparseShiftedAndInputOrder) {
    Grid grid = makeGrid({-10.5,-8.25,12.5,11.75},.5);
    std::vector<cache::Measurement> points;
    for (int i = 0; i <= 200; ++i) {
        points.push_back({-10+i*.1,-2,0}); points.push_back({1,-8+i*.09,0});
        points.push_back({-9.875+i*.1,-1.875,0});
    }
    for (int i = 0; i < 100; ++i) points.push_back({9.75,9.75,0});
    for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) points.push_back({-10.25+x*.5,9.25+y*.5,0});
    const auto expected = oracleCounts(grid,points);
    for (int order = 0; order < 2; ++order) {
        QTemporaryDir root; if (order) std::reverse(points.begin(),points.end()); auto path = writeCache(root,points);
        for (auto group : {1U,2U,4U}) for (auto candidate : {Candidate::Sequential,Candidate::Spatial}) {
            Parameters p; p.grid = grid; p.tileSide = 4; p.groupTiles = group; p.eventCapacity = order ? 11 : 64;
            EXPECT_EQ(run(path,root,p,candidate,QString("-%1-%2").arg(group).arg(int(candidate))),expected);
        }
    }
}
TEST(DensityConstruction, RandomBoundaryMappingAgreesWithIntervalOracle) {
    std::mt19937_64 random(6003);
    for (double origin : {-1000.125,-.3,0.,1e12}) for (double step : {.25,.7,2.}) {
        const auto grid = makeGrid({origin,origin,origin+9,origin+7},step); validateGrid(grid,noop);
        for (int i = 0; i < 1000; ++i) {
            cadcontour::ModelPoint point{origin+double(random()%900001)/100000.,origin+double(random()%700001)/100000.};
            const auto k = cell(grid,point); EXPECT_EQ(k.y*grid.width+k.x,oracleCell(grid,point));
        }
    }
}
TEST(DensityConstruction, HugeEmptyRegionsHaveNoFullRasterAllocation) {
    QTemporaryDir root; std::vector<cache::Measurement> points{{0,0,0},{999999,0,0},{999999,0,0}};
    auto path = writeCache(root,points); Parameters p; p.grid = makeGrid({0,0,999999,0},1); p.tileSide = 256; p.groupTiles = 4;
    for (auto candidate : {Candidate::Sequential,Candidate::Spatial}) {
        auto reader = take(cache::CacheReader::open(path,{8*MiB,0,1})); FileIo io; U64 tiles = 0,pointsSeen = 0;
        auto result = build(candidate,*reader,p,root.filePath(QString::number(int(candidate))),[&](const Tile &t) { ++tiles; pointsSeen += sum(t); },io,noop);
        EXPECT_EQ(tiles,2); EXPECT_EQ(pointsSeen,3); EXPECT_EQ(integer(result,"groupBufferBytes"),8*1024*1024);
    }
}
TEST(DensityConstruction, InclusiveNeighboursAreFilteredAndStatisticsSummed) {
    QTemporaryDir root; auto grid = makeGrid({0,0,4,4},1);
    auto path = writeCache(root,{{0,0,0},{2,2,0},{2,2,0},{4,4,0}}); Parameters p; p.grid = grid; p.tileSide = 1; p.groupTiles = 2;
    auto reader = take(cache::CacheReader::open(path,{8*MiB,0,1})); FileIo io;
    auto result = build(Candidate::Spatial,*reader,p,root.filePath("scratch"),[](const Tile &) {},io,noop);
    EXPECT_EQ(integer(result,"seenPoints"),4); EXPECT_EQ(integer(result,"queries"),4);
    EXPECT_GT(integer(result,"boundaryNeighbourRecordsRejected"),0);
    EXPECT_EQ(integer(result["cacheReaderScans"].toObject(),"pointPages"),4);
    EXPECT_GT(integer(result["cacheReaderScans"].toObject(),"returnedRecords"),4);
}
TEST(DensityConstruction, ResourceRefusalAndCollapsedBinary64Grid) {
    Parameters p; p.grid = makeGrid({0,0,10,10},1); p.tileSide = 2; p.groupTiles = 1; p.maxGroups = 2;
    EXPECT_THROW(layout(p),Failure); p.maxGroups = 100; p.groupTiles = 65535;
    EXPECT_THROW(layout(p),Failure);
    EXPECT_THROW(validateGrid(makeGrid({1e16,0,1e16+4,0},1),noop),Failure);
    QTemporaryDir root; auto path = writeCache(root,{{0,0,0}}); auto reader = take(cache::CacheReader::open(path,{8*MiB,0,1}));
    p.groupTiles = 1; p.limits.ramBytes = 1; FileIo io;
    try { build(Candidate::Sequential,*reader,p,root.filePath("scratch"),[](const Tile &) {},io,noop); FAIL(); }
    catch (const Failure &e) { EXPECT_EQ(e.status,"resource_refused"); }
    EXPECT_FALSE(QFileInfo::exists(root.filePath("scratch")));
    p.limits.ramBytes = 512*MiB; p.limits.temporaryBytes = 1;
    EXPECT_THROW(build(Candidate::Sequential,*reader,p,root.filePath("scratch2"),[](const Tile &) {},io,noop),Failure);
}
TEST(DensityConstruction, CancellationAndSinkIoFailureNeverCompleteOrModifySource) {
    for (auto candidate : {Candidate::Sequential,Candidate::Spatial}) for (int stop : {1,5,20}) {
        QTemporaryDir root; std::vector<cache::Measurement> points(100,{.5,.5,0}); auto path = writeCache(root,points);
        FileIo io; const auto before = readFile(path,MiB,io); auto reader = take(cache::CacheReader::open(path,{8*MiB,0,1}));
        Parameters p; p.grid = makeGrid({0,0,10,10},1); p.tileSide = 1; p.groupTiles = 2; p.eventCapacity = 3;
        Output out(root.filePath("partial"),layout(p).spec,100,p.limits.outputBytes,io); int calls = 0;
        try {
            build(candidate,*reader,p,root.filePath("scratch"),[&](const Tile &t) { out.append(t); },io,
                [&] { if (++calls == stop) throw Failure("cancelled","test"); });
            if (calls < stop) GTEST_FAIL() << "cancellation checkpoint not reached";
            FAIL();
        } catch (const Failure &e) { EXPECT_EQ(e.status,"cancelled"); }
        EXPECT_THROW(out.verify({},noop),Failure); EXPECT_EQ(readFile(path,MiB,io),before);
        if (QDir(root.filePath("scratch")).exists()) EXPECT_EQ(directoryBytes(root.filePath("scratch")),0);
    }
}
TEST(DensityConstruction, ReaderCorruptionAndOutputIoErrorsAreFailures) {
    QTemporaryDir root; auto path = writeCache(root,{{0,0,0},{4,4,0}}); FileIo io;
    auto before = readFile(path,MiB,io); const auto corrupt = root.filePath("corrupt.pc2dcache"); auto broken = before; broken[8192+100] ^= 1; writeNew(corrupt,broken,io);
    Parameters p; p.grid = makeGrid({0,0,4,4},1); p.tileSide = 1; p.groupTiles = 2;
    for (auto candidate : {Candidate::Sequential,Candidate::Spatial}) {
        auto reader = take(cache::CacheReader::open(corrupt,{8*MiB,0,1}));
        EXPECT_THROW(build(candidate,*reader,p,root.filePath("bad"+QString::number(int(candidate))),[](const Tile &) {},io,noop),Failure);
        auto good = take(cache::CacheReader::open(path,{8*MiB,0,1}));
        try { build(candidate,*good,p,root.filePath("sink"+QString::number(int(candidate))),[](const Tile &) { throw Failure("io_error","injected sink write failure"); },io,noop); FAIL(); }
        catch (const Failure &e) { EXPECT_EQ(e.status,"io_error"); }
    }
    EXPECT_THROW(Output(root.filePath("missing/counts"),layout(p).spec,2,p.limits.outputBytes,io),Failure);
    Output small(root.filePath("small"),layout(p).spec,2,1,io); EXPECT_THROW(small.append(Tile{{0,0},1,1,{1}}),Failure);
    EXPECT_EQ(readFile(path,MiB,io),before);
    QFile shortFile(root.filePath("truncated")); ASSERT_TRUE(shortFile.open(QIODevice::WriteOnly)); shortFile.write(before.first(8192)); shortFile.close();
    EXPECT_THROW(take(cache::CacheReader::open(shortFile.fileName(),{8*MiB,0,1})),Failure);
}
TEST(DensityConstruction, LateCancellationAfterEmittedPrefixAndRealSpoolOpenFailure) {
    for (auto candidate : {Candidate::Sequential,Candidate::Spatial}) {
        QTemporaryDir root; auto path = writeCache(root,{{0,0,0},{3,3,0}});
        Parameters p; p.grid = makeGrid({0,0,4,4},1); p.tileSide = 1; p.groupTiles = 2;
        auto reader = take(cache::CacheReader::open(path,{8*MiB,0,1})); FileIo io; bool prefix = false;
        Output out(root.filePath("partial"),layout(p).spec,2,p.limits.outputBytes,io);
        try {
            build(candidate,*reader,p,root.filePath("scratch"),[&](const Tile &t) { out.append(t); prefix = true; },io,
                [&] { if (prefix) throw Failure("cancelled","late cancellation"); });
            FAIL();
        } catch (const Failure &e) { EXPECT_EQ(e.status,"cancelled"); }
        EXPECT_TRUE(prefix); EXPECT_THROW(out.verify({},noop),Failure);
        EXPECT_EQ(directoryBytes(root.filePath("scratch")),0);
    }
    QTemporaryDir root; auto path = writeCache(root,{{0,0,0}});
    Parameters p; p.grid = makeGrid({0,0,0,0},1); p.tileSide = 1; p.groupTiles = 1; p.eventCapacity = 1;
    auto reader = take(cache::CacheReader::open(path,{8*MiB,0,1})); FileIo io; bool injected = false;
    try {
        build(Candidate::Sequential,*reader,p,root.filePath("scratch"),[](const Tile &) {},io,[&] {
            if (injected) return;
            QDir scratch(root.filePath("scratch")); if (!scratch.exists()) return;
            const auto dirs = scratch.entryList({"events-*"},QDir::Dirs | QDir::NoDotAndDotDot);
            if (dirs.isEmpty()) return;
            injected = QDir().mkdir(scratch.filePath(dirs[0]+"/0.events"));
        });
        FAIL();
    } catch (const Failure &e) { EXPECT_EQ(e.status,"io_error"); }
    EXPECT_TRUE(injected); EXPECT_EQ(directoryBytes(root.filePath("scratch")),0);
}
TEST(DensityConstruction, OutputCanonicalDigestIndependentOfGroupOrderAndExactReadback) {
    QTemporaryDir root; FileIo io; Spec spec{"raster-counts",3,3,2,0,true};
    const std::vector<Tile> tiles{{{0,0},2,2,{1,0,0,7}},{{1,1},1,1,{4}}};
    QJsonObject first;
    for (int reverse = 0; reverse < 2; ++reverse) {
        Output out(root.filePath(QString::number(reverse)),spec,12,MiB,io);
        if (reverse) { out.append(tiles[1]); out.append(tiles[0]); } else for (auto &t : tiles) out.append(t);
        out.finish(12); auto result = out.verify({1,0,0,0,7,0,0,0,4},noop);
        if (!reverse) first = result; else EXPECT_EQ(result,first);
        EXPECT_THROW(out.verify(std::vector<U64>(9,1),noop),Failure);
    }
}
TEST(DensityConstruction, CliBalancedWorkersRefusalAndExistingDirectoryPreserved) {
    QTemporaryDir root; auto path = writeCache(root,{{0,0,0},{2,2,0},{2,2,0},{4,4,0}});
    auto invoke = [&](QStringList args) { QProcess p; p.start(DENSITY_CONSTRUCTION_EXE,args); if (!p.waitForFinished(60000)) { p.kill(); p.waitForFinished(); return -1; } return p.exitCode(); };
    EXPECT_EQ(invoke({"--help"}),0);
    auto dir = root.filePath("run");
    EXPECT_EQ(invoke({"--cache",path,"--output-dir",dir,"--repeats","2","--tile-size","1","--group-tiles","2"}),0);
    FileIo io; auto result = QJsonDocument::fromJson(readFile(QDir(dir).filePath("results.json"),MiB,io)).object();
    EXPECT_TRUE(result["success"].toBool()); EXPECT_EQ(result["measurements"].toArray().size(),8);
    EXPECT_EQ(invoke({"--cache",path,"--output-dir",root.path()}),1);
    EXPECT_FALSE(QFileInfo::exists(root.filePath("results.json")));
    EXPECT_EQ(invoke({"--cache",path,"--output-dir",root.filePath("refused"),"--ram-mib","1"}),1);
    auto refusal = QJsonDocument::fromJson(readFile(root.filePath("refused/results.json"),MiB,io)).object(); EXPECT_EQ(refusal["status"],"resource_refused");
}
}
