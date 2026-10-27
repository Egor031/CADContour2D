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
        require(argc == 2,"usage: density_construction_prepare NEW_DIRECTORY");
        const auto dir = QFileInfo(app.arguments()[1]).absoluteFilePath();
        require(!QFileInfo::exists(dir) && QDir().exists(QFileInfo(dir).absolutePath()),"new directory with existing parent required");
        preflight(QFileInfo(dir).absolutePath(),{512*MiB,256*MiB,256*MiB},{64*MiB,128*MiB,128*MiB});
        require(QDir().mkdir(dir),"cannot create preparation directory"); FileIo io;
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
