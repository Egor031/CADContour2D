#include "construction.h"
#include "generator.h"
#include "cache/cache_writer.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QJsonDocument>
#include <cstdio>

using namespace density;
using namespace density::construction;
namespace cache = cadcontour::cache;
// Small additional datasets from existing 0.4.0 fixtures. Preparation and oracle
// costs are deliberately outside candidate measurements. No new generator schema.
int main(int argc,char **argv) {
    QCoreApplication app(argc,argv);
    try {
        require(argc == 2 || (argc == 3 && app.arguments()[2] == "--p1"),"usage: density_construction_prepare NEW_DIRECTORY [--p1]");
        const auto dir = QFileInfo(app.arguments()[1]).absoluteFilePath();
        require(!QFileInfo::exists(dir) && QDir().exists(QFileInfo(dir).absolutePath()),"new directory with existing parent required");
        preflight(QFileInfo(dir).absolutePath(),{512*MiB,256*MiB,256*MiB},{64*MiB,128*MiB,128*MiB});
        require(QDir().mkdir(dir),"cannot create preparation directory"); FileIo io;
        if (argc == 3) {
            QJsonArray jobs;
            for (U64 groups : {0ULL,64ULL,1024ULL,4096ULL}) {
                const auto name = groups ? "occupied-"+decimal(groups) : QString("four-clusters");
                const auto path = QDir(dir).filePath(name+".pc2dcache");
                auto writer = take(cache::CacheWriter::create(path,{8*MiB,128*MiB,4},dir));
                std::vector<cache::Measurement> batch; batch.reserve(4096);
                U64 axis = 1; while (axis*axis < groups) ++axis;
                for (U64 i = 0; i < 262144; ++i) {
                    double x, y;
                    if (!groups) {
                        const U64 cluster = i/65536, local = i%65536;
                        x = double((cluster%2)*16128+local%256); y = double((cluster/2)*16128+local/256);
                    } else {
                        const U64 group = i%groups;
                        x = double((group%axis)*32)+.5; y = double((group/axis)*32)+.5;
                    }
                    batch.push_back({x,y,0});
                    if (batch.size() == 4096) { take(writer->append(batch)); batch.clear(); }
                }
                auto pending = take(writer->finish()); pending.releaseToOwner();
                jobs.append(QJsonObject{{"name",name},{"cachePath",path},{"pointCount","262144"},{"occupiedFixtureGroups",decimal(groups)}});
            }
            writeNew(QDir(dir).filePath("p1-inputs.json"),QJsonDocument(QJsonObject{{"fixtures",jobs}}).toJson(),io);
            return 0;
        }
        const QString fixtures = QStringLiteral(DENSITY_SOURCE_DIR "/tests/fixtures/synthetic/");
        const auto geometryBytes = readFile(fixtures+"geometry/rectangle.json",MiB,io);
        const auto geometry = synthetic::parseGeometry(geometryBytes);
        QJsonArray jobs;
        for (const QString &name : {"clean_vertical","clean_horizontal_vertical","shifted_passes","overlapping_passes","double_scan","outside_grid_cloud","dense-square"}) {
            auto scanBytes = readFile(fixtures+"scans/"+(name == "dense-square" ? "clean_horizontal" : name)+".json",MiB,io);
            if (name == "dense-square") {
                auto scan = QJsonDocument::fromJson(scanBytes).object(); scan["pointStep"] = .02; scan["lineStep"] = .2;
                scanBytes = QJsonDocument(scan).toJson();
            }
            const auto grid = name == "dense-square" ? makeGrid({0,0,100,60},2) : makeGrid({-5,-5,170,65},5);
            const auto scenario = synthetic::parseScan(scanBytes); const auto path = QDir(dir).filePath(name+".pc2dcache");
            auto writer = take(cache::CacheWriter::create(path,{8*MiB,128*MiB,4},dir));
            std::vector<U64> expected(std::size_t(grid.width*grid.height)); std::vector<cache::Measurement> batch; batch.reserve(1024);
            U64 count = 0;
            synthetic::generate(geometry,scenario,[&](synthetic::Point point) {
                const auto label = oracleCell(grid,{point.x,point.y}); expected[label] = add(expected[label],1);
                batch.push_back({point.x,point.y,0}); ++count;
                if (batch.size() == 1024) { take(writer->append(batch)); batch.clear(); }
            });
            take(writer->append(batch)); auto pending = take(writer->finish()); pending.releaseToOwner();
            jobs.append(QJsonObject{{"name",name},{"cachePath",path},{"grid",toJson(grid)},{"pointCount",decimal(count)},
                {"expectedCounts",countsJson(expected)},{"oracleAvailable",true},{"generatorVersion",synthetic::generatorVersion},
                {"geometrySha256",QString::fromLatin1(QCryptographicHash::hash(geometryBytes,QCryptographicHash::Sha256).toHex())},
                {"scanSha256",QString::fromLatin1(QCryptographicHash::hash(scanBytes,QCryptographicHash::Sha256).toHex())},
                {"effectiveScan",QJsonDocument::fromJson(scanBytes).object()}});
        }
        writeNew(QDir(dir).filePath("inputs.json"),QJsonDocument(QJsonObject{{"rasters",jobs},{"schema","density-experiment-inputs-1"}}).toJson(),io);
        return 0;
    } catch (const std::exception &e) { std::fprintf(stderr,"%s\n",e.what()); return 1; }
}
