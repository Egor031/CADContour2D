#include "construction.h"
#include <QCommandLineParser>
#include <QCommandLineOption>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QJsonDocument>
#include <QProcess>
#include <QtEndian>
#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdio>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

using namespace density;
using namespace density::construction;
namespace cache = cadcontour::cache;
namespace {
std::atomic<bool> interrupted = false;
BOOL WINAPI interrupt(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT) { interrupted = true; return TRUE; } return FALSE;
}
void check() { if (interrupted) throw Failure("cancelled","experiment interrupted"); }
QJsonObject json(const QString &path) {
    FileIo io; QJsonParseError e; auto doc = QJsonDocument::fromJson(readFile(path,64*MiB,io),&e);
    require(e.error == QJsonParseError::NoError && doc.isObject(),"invalid JSON: "+path); return doc.object();
}
U64 argument(const QCommandLineParser &p, const char *key) {
    bool ok = false; auto n = p.value(key).toULongLong(&ok); require(ok,QString("invalid --%1").arg(key)); return n;
}
QString fileHash(const QString &path) {
    QFile f(path); if (!f.open(QIODevice::ReadOnly)) throw Failure("io_error",f.errorString());
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!f.atEnd()) { check(); auto b = f.read(4*MiB); if (b.isEmpty() && f.error() != QFileDevice::NoError) throw Failure("io_error",f.errorString()); hash.addData(b); }
    return QString::fromLatin1(hash.result().toHex());
}
// Existing Save acceptance fixture: independent separable counts, then verify
// every RecordId/coordinate against its generator. No construction mapping used.
std::vector<U64> regularReference(cache::CacheReader &reader, const Grid &grid) {
    const U64 n = reader.metadata().pointCount;
    require(n && !(n%100000) && n <= 100000000,"not a regular Save acceptance fixture size");
    require(multiply(grid.width,grid.height) <= 2000000,"regular reference limited to 2M cells");
    std::vector<bool> seen(std::size_t(n),false); U64 count = 0;
    take(reader.scanAll([&](auto batch) -> cache::Status {
        check(); for (auto r : batch) {
            require(r.id < n && !seen[std::size_t(r.id)] && r.point.x == double(r.id%100000)*.01
                && r.point.y == double(r.id/100000)*.01,"regular fixture identity/coordinate mismatch");
            seen[std::size_t(r.id)] = true; ++count;
        } return cache::success();
    }));
    require(count == n,"regular fixture incomplete");
    auto histogram = [&](double origin,double maximum,U64 cells,U64 values) {
        std::vector<double> coords; coords.reserve(std::size_t(values));
        for (U64 i = 0; i < values; ++i) coords.push_back(double(i)*.01);
        std::vector<U64> out(std::size_t(cells),0);
        for (U64 i = 0; i < cells; ++i) {
            const double lo = origin+double(i)*grid.cell, hi = origin+double(i+1)*grid.cell;
            const auto first = std::lower_bound(coords.begin(),coords.end(),lo);
            const auto last = i+1 == cells ? std::upper_bound(coords.begin(),coords.end(),maximum)
                : std::lower_bound(coords.begin(),coords.end(),hi);
            require(last >= first,"invalid reference interval"); out[std::size_t(i)] = U64(last-first);
        } return out;
    };
    auto x = histogram(grid.bounds.minX,grid.bounds.maxX,grid.width,100000);
    auto y = histogram(grid.bounds.minY,grid.bounds.maxY,grid.height,n/100000);
    std::vector<U64> counts; counts.reserve(std::size_t(grid.width*grid.height));
    for (auto ny : y) for (auto nx : x) counts.push_back(multiply(nx,ny));
    return counts;
}
}
int main(int argc,char **argv) {
    QCoreApplication app(argc,argv); SetConsoleCtrlHandler(interrupt,TRUE);
    QCommandLineParser cli; cli.setApplicationDescription("06.0C isolated count construction experiment"); cli.addHelpOption();
    cli.addOptions({{"output-dir","New isolated directory (parent must exist).","path"},
        {"inputs","06.0A inputs.json (rasters).","path"},{"cache","Existing immutable cache.","path"},
        {"cell","Cell mm for --cache.","mm","1"},{"pad-x","Extend minimum X by this many cells.","cells","0"},
        {"pad-y","Extend minimum Y by this many cells.","cells","0"},
        {"tile-size","Tile side 1..1024.","n","256"},{"group-tiles","Square group side in tiles.","n","2"},
        {"event-capacity","Bounded C1 events per chunk.","n","65536"},{"max-groups","C2 logical query guard.","n","1000000"},
        {"max-occupied-groups","Occupied groups / C1 spool files guard (planned upper bound).","n","1000000"},
        {"max-boundary-checks","Experimental axis validation work guard.","n","100000000"},
        {"ram-mib","Common RAM budget.","n","512"},{"temp-mib","Common scratch budget.","n","1024"},
        {"output-mib","Common output budget.","n","1024"},{"repeats","Balanced fresh-process pairs.","n","4"},
        {"storage-label","Verified storage description.","text"},{"regular-fixture","Verify known Save acceptance fixture and exact separable counts."},
        {"worker","Internal single candidate worker.","candidate"},{"job","Internal prepared raster job JSON.","path"}});
    if (!cli.parse(app.arguments())) { std::fprintf(stderr,"%s\n",qPrintable(cli.errorText())); return 2; }
    if (cli.isSet("help")) { std::printf("%s",qPrintable(cli.helpText())); return 0; }
    QString dir; bool ownsDirectory = false; QJsonObject report; QJsonArray rows;
    try {
        require(cli.isSet("output-dir"),"--output-dir required"); dir = QFileInfo(cli.value("output-dir")).absoluteFilePath();
        require(!QFileInfo::exists(dir) && QDir().exists(QFileInfo(dir).absolutePath()) && QDir().mkdir(dir),"output directory must be new with existing parent");
        ownsDirectory = true;
        Parameters p; const auto side = argument(cli,"tile-size"), group = argument(cli,"group-tiles");
        require(side && side <= 1024 && group && group <= 65535,"invalid tile/group size");
        p.tileSide = std::uint32_t(side); p.groupTiles = std::uint32_t(group);
        p.eventCapacity = argument(cli,"event-capacity"); p.maxGroups = argument(cli,"max-groups");
        p.maxOccupiedGroups = argument(cli,"max-occupied-groups");
        p.maxBoundaryChecks = argument(cli,"max-boundary-checks");
        p.limits = {multiply(argument(cli,"ram-mib"),MiB),multiply(argument(cli,"temp-mib"),MiB),multiply(argument(cli,"output-mib"),MiB)};
        report = {{"schema","density-construction-06c-1"},{"environment",environment(DENSITY_SOURCE_DIR,dir,cli.value("storage-label"))}};
        auto checkpoint = [&] { check(); if (workingSet() > p.limits.ramBytes) throw Failure("resource_refused","observed working set above budget"); };
        FileIo metaIo;
        if (cli.isSet("worker")) {
            const auto candidate = cli.value("worker"); require(candidate == "C1" || candidate == "C2","unknown worker");
            auto job = json(cli.value("job")); p.grid = gridFromJson(job["grid"].toObject());
            const auto path = job["cachePath"].toString(), name = job["name"].toString();
            auto expected = countsFromJson(QJsonArray{});
            for (auto v : job["expectedCounts"].toArray()) expected.push_back(integer({{"n",v}},"n"));
            std::unique_ptr<cache::CacheReader> reader;
            auto open = measure("open",name,dir,[&](FileIo &) {
                checkpoint(); reader = take(cache::CacheReader::open(path,{8*MiB,0,1}));
                auto pre = preflight(dir,p.limits,plan(p,reader->metadata().pointCount));
                return QJsonObject{{"cacheReaderOpen",cacheStatistics(reader->statistics())},{"preflight",pre}};
            }); open["candidate"] = candidate; rows.append(open);
            if (open["status"] != "ok") throw Failure(open["status"].toString(),open["diagnostic"].toString());
            std::unique_ptr<Output> out; QJsonObject result;
            auto row = measure("build_counts",name,dir,[&](FileIo &io) {
                out = std::make_unique<Output>(QDir(dir).filePath("counts.partial"),layout(p).spec,reader->metadata().pointCount,p.limits.outputBytes,io,
                    multiply(p.maxOccupiedGroups,multiply(p.groupTiles,p.groupTiles)));
                auto details = build(candidate == "C1" ? Candidate::Sequential : Candidate::Spatial,*reader,p,
                    QDir(dir).filePath("scratch"),[&](const Tile &tile) { out->append(tile); },io,checkpoint);
                out->finish(reader->metadata().pointCount); details["outputAndFinalizationMs"] = out->outputMs;
                return details;
            },[&] { result = out->verify(expected,checkpoint); return integer(result,"sum") == reader->metadata().pointCount; });
            row["candidate"] = candidate; auto details = row["details"].toObject(); details["fingerprint"] = result; row["details"] = details; rows.append(row);
            report["success"] = row["status"] == "ok"; report["measurements"] = rows;
            if (row["status"] == "ok") writeNew(QDir(dir).filePath("complete.json"),QJsonDocument(result).toJson(),metaIo);
        } else {
            require(cli.isSet("inputs") != cli.isSet("cache"),"exactly one --inputs or --cache required");
            const U64 repeats = argument(cli,"repeats"); require(repeats >= 2 && repeats <= 20,"repeats must be 2..20");
            QJsonArray jobs;
            if (cli.isSet("inputs")) jobs = json(cli.value("inputs"))["rasters"].toArray();
            else {
                bool ok = false; auto value = cli.value("cell").toDouble(&ok); require(ok,"invalid cell");
                const auto path = QFileInfo(cli.value("cache")).absoluteFilePath();
                auto reader = take(cache::CacheReader::open(path,{8*MiB,0,1})); auto b = reader->metadata().bounds;
                bool xok = false,yok = false; double px = cli.value("pad-x").toDouble(&xok), py = cli.value("pad-y").toDouble(&yok);
                require(xok && yok && px >= 0 && py >= 0,"invalid grid padding"); b.minX -= px*value; b.minY -= py*value;
                p.grid = makeGrid(b,value); preflight(dir,p.limits,plan(p,reader->metadata().pointCount)); validateGrid(p.grid,checkpoint);
                QJsonObject job{{"name",QFileInfo(path).baseName()},{"cachePath",path},{"grid",toJson(p.grid)}};
                if (cli.isSet("regular-fixture")) { job["expectedCounts"] = countsJson(regularReference(*reader,p.grid)); job["reference"] = "independent separable interval histograms + every RecordId/coordinate checked"; }
                jobs.append(job);
            }
            require(!jobs.isEmpty(),"no raster jobs"); QJsonArray sources;
            Plan series;
            for (auto v : jobs) {
                auto job = v.toObject(); p.grid = gridFromJson(job["grid"].toObject());
                auto reader = take(cache::CacheReader::open(job["cachePath"].toString(),{8*MiB,0,1}));
                auto demand = plan(p,reader->metadata().pointCount);
                series.ramBytes = std::max(series.ramBytes,demand.ramBytes);
                series.temporaryBytes = std::max(series.temporaryBytes,demand.temporaryBytes);
                series.outputBytes = add(series.outputBytes,multiply(multiply(repeats,2),demand.outputBytes));
            }
            report["seriesPreflight"] = preflight(dir,p.limits,series);
            for (qsizetype j = 0; j < jobs.size(); ++j) {
                auto job = jobs[j].toObject(); auto path = job["cachePath"].toString(); p.grid = gridFromJson(job["grid"].toObject());
                auto reader = take(cache::CacheReader::open(path,{8*MiB,0,1})); const auto count = reader->metadata().pointCount;
                const auto pre = preflight(dir,p.limits,plan(p,count)); reader.reset(); checkpoint();
                const auto beforeHash = fileHash(path);
                sources.append(QJsonObject{{"path",path},{"bytes",decimal(U64(QFileInfo(path).size()))},{"sha256Before",beforeHash},{"preflight",pre},
                    {"grid",job["grid"]},{"oracleAvailable",job.contains("expectedCounts")},{"pointCount",decimal(count)}});
                const auto jobPath = QDir(dir).filePath(QString("job-%1.json").arg(j)); writeNew(jobPath,QJsonDocument(job).toJson(),metaIo);
                QString baseline;
                for (U64 repeat = 0; repeat < repeats; ++repeat) {
                    const QStringList order = repeat%2 ? QStringList{"C2","C1"} : QStringList{"C1","C2"};
                    for (const auto &candidate : order) {
                        checkpoint(); const auto childDir = QDir(dir).filePath(QString("job-%1-r%2-%3").arg(j).arg(repeat).arg(candidate));
                        QStringList args{"--worker",candidate,"--job",jobPath,"--output-dir",childDir,"--tile-size",decimal(p.tileSide),"--group-tiles",decimal(p.groupTiles),
                            "--event-capacity",decimal(p.eventCapacity),"--max-groups",decimal(p.maxGroups),"--max-occupied-groups",decimal(p.maxOccupiedGroups),
                            "--max-boundary-checks",decimal(p.maxBoundaryChecks),"--ram-mib",decimal(p.limits.ramBytes/MiB),
                            "--temp-mib",decimal(p.limits.temporaryBytes/MiB),"--output-mib",decimal(p.limits.outputBytes/MiB),"--storage-label",cli.value("storage-label")};
                        QProcess child; child.start(QCoreApplication::applicationFilePath(),args); require(child.waitForStarted(),"worker did not start");
                        while (!child.waitForFinished(100)) { if (interrupted) { child.kill(); child.waitForFinished(5000); check(); } }
                        auto worker = json(QDir(childDir).filePath("results.json"));
                        for (auto v : worker["measurements"].toArray()) {
                            auto r = v.toObject(); r["repetition"] = int(repeat); r["runClass"] = repeat ? "subsequent" : "first";
                            r["workerDirectory"] = childDir; rows.append(r);
                            require(r["status"] == "ok" && (r["phase"] == "open" || r["correctness"].toBool()),"worker measurement failed");
                            if (r["phase"] == "build_counts") {
                                const auto digest = r["details"].toObject()["fingerprint"].toObject()["sha256"].toString();
                                require(!digest.isEmpty() && (baseline.isEmpty() || baseline == digest),"paired/repeated digest mismatch"); baseline = digest;
                            }
                        }
                        require(child.exitCode() == 0 && worker["success"].toBool(),"worker failed: "+QString::fromUtf8(child.readAllStandardError()));
                    }
                }
                const auto afterHash = fileHash(path); require(beforeHash == afterHash,"source cache changed");
                auto source = sources.last().toObject(); source["sha256After"] = afterHash; sources[sources.size()-1] = source;
            }
            report["success"] = true; report["sources"] = sources; report["measurements"] = rows; report["summarySubsequent"] = summary(rows);
            report["order"] = "r0 C1/C2, r1 C2/C1 alternating; fresh worker each; inputs/reference/SHA scans warm OS cache; first is not cold";
        }
        writeNew(QDir(dir).filePath("results.json"),QJsonDocument(report).toJson(),metaIo);
        writeNew(QDir(dir).filePath("results.csv"),csv(rows),metaIo);
        return report["success"].toBool() ? 0 : 1;
    } catch (const Failure &e) {
        std::fprintf(stderr,"%s: %s\n",qPrintable(e.status),e.what());
        if (ownsDirectory && !QFileInfo::exists(QDir(dir).filePath("results.json"))) {
            report["success"] = false; report["status"] = e.status; report["diagnostic"] = e.what(); report["measurements"] = rows;
            try { FileIo io; writeNew(QDir(dir).filePath("results.json"),QJsonDocument(report).toJson(),io); } catch (...) { return 2; }
        }
        return 1;
    } catch (const std::exception &e) { std::fprintf(stderr,"%s\n",e.what()); return 2; }
}
