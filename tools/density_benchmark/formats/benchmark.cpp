#include "formats.h"
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <atomic>
#include <cstdio>
#include <type_traits>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

using namespace density;
using namespace density::formats;
namespace {
std::atomic<bool> cancelled = false;
BOOL WINAPI interrupt(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT) { cancelled = true; return TRUE; }
    return FALSE;
}
U64 option(const QCommandLineParser &p, const char *name) {
    bool ok = false; auto value = p.value(name).toULongLong(&ok);
    require(ok,QString("invalid integer option: %1").arg(name)); return value;
}
void save(const QString &path, const QJsonObject &j) {
    FileIo io; writeNew(path,QJsonDocument(j).toJson(),io);
}
QJsonObject load(const QString &path) {
    FileIo io; QJsonParseError e;
    auto doc = QJsonDocument::fromJson(readFile(path,32*MiB,io),&e);
    require(e.error == QJsonParseError::NoError && doc.isObject(),"invalid worker result"); return doc.object();
}
TrackedFile &tracked(DReader &r) { return r.file; }
TrackedFile &tracked(EReader &r) { return r.tracked(); }
struct Totals {
    U64 cells = 0, sum = 0, tiles = 0;
    void take(const Tile &t) { cells = add(cells,t.counts.size()); sum = add(sum,density::sum(t)); ++tiles; }
    bool operator==(const Totals &) const = default;
};
template<class Reader> void trial(const QString &dir, const Spec &s, Limits limits, QJsonArray &rows) {
    const bool tiff = std::is_same_v<Reader,EReader>;
    const auto plan = storagePlan(s,tiff); preflight(dir,limits,plan);
    auto check = [&] { if (cancelled) throw Failure("cancelled","experiment cancelled");
        if (workingSet() > limits.ramBytes) throw Failure("resource_refused","process RAM budget exceeded"); };
    auto source = [&](TileKey k) { check(); return generate(s,k); };
    const auto expected = fingerprint(s,source);
    const auto path = QDir(dir).filePath(tiff ? "counts.tif" : "counts.dct");
    auto push = [&](QJsonObject row) { rows.append(row);
        if (row["status"] != "ok") throw Failure(row["status"].toString(),row["diagnostic"].toString("verification failed")); };
    Totals generated;
    push(measure("source_generate",s.name,dir,[&](FileIo &) {
        visit(s,{},[&](TileKey k) { generated.take(source(k)); });
        return QJsonObject{{"cells",decimal(generated.cells)},{"sum",decimal(generated.sum)}};
    },[&] { return generated.sum == integer(expected,"sum"); }));
    push(measure("write_finalize",s.name,dir,[&](FileIo &io) {
        if constexpr (std::is_same_v<Reader,DReader>) writeD(path,s,source,io,check);
        else writeE(path,s,source,io,check);
        return QJsonObject{{"fileBytes",decimal(U64(QFileInfo(path).size()))},
            {"uncompressedLogicalBytes",decimal(multiply(multiply(s.width,s.height),s.wide ? 8 : 4))},
            {"source","06.0A generate; generation/packing included; verify excluded"}};
    },[&] {
        Reader r(path,s,limits.ramBytes/2);
        return fingerprint(s,[&](TileKey k) { check(); auto t = r.read(k);
            require(t == generate(s,k),"round-trip differs from deterministic reference"); return t; }) == expected;
    }));
    std::unique_ptr<Reader> reader;
    push(measure("open_index",s.name,dir,[&](FileIo &io) {
        reader = std::make_unique<Reader>(path,s,limits.ramBytes/2); io = tracked(*reader).io;
        const auto bytes = U64(QFileInfo(path).size());
        return QJsonObject{{"metadataIndexBytes",decimal(reader->metadataBytes())},
            {"fileBytes",decimal(bytes)},{"physicalTiles",decimal(reader->physicalKeys().size())},
            {"logicalTiles",decimal(logicalTiles(s))},{"fingerprint",expected},
            {"estimatedPlan",QJsonObject{{"ramBytes",decimal(plan.ramBytes)},{"outputBytes",decimal(plan.outputBytes)}}},
            {"allocationTrace",QJsonValue::Null}};
    },[&] { return reader->metadataBytes() > 0; }));
    auto plans = readPlans(s);
    plans.push_back({"roi-large",{Access::Roi,{},s.width/4,s.height/4,s.width-s.width/4,s.height-s.height/4}});
    ReadPlan random{Access::Tiles};
    U64 state = s.seed;
    for (int i = 0; i < 128; ++i) {
        state = state*6364136223846793005ULL+1442695040888963407ULL;
        random.tiles.push_back(keyAt(s,state%tileCount(s)));
    }
    plans.push_back({"random-128",random});
    auto readKeys = [&](const std::function<void(const std::function<void(TileKey)> &)> &keys, Totals &total, bool verify) {
        keys([&](TileKey k) { check(); auto tile = reader->read(k);
            if (verify) require(tile == generate(s,k),"tile values/address/shape differ from reference");
            total.take(tile); });
    };
    for (const auto &[name,request] : plans) {
        Totals got;
        push(measure("read_"+name,s.name,dir,[&](FileIo &io) {
            tracked(*reader).io = {};
            readKeys([&](const auto &sink) { visit(s,request,sink); },got,false);
            io = tracked(*reader).io;
            return QJsonObject{{"request",toJson(request)},{"returnedCells",decimal(got.cells)},
                {"returnedTiles",decimal(got.tiles)},{"sum",decimal(got.sum)},
                {"coverage",s.name == "huge-sparse" && request.access != Access::Tiles
                    ? "source explicit tiles; implicit zero complement skipped" : "logical requested tiles"}};
        },[&] { Totals verified;
            readKeys([&](const auto &sink) { visit(s,request,sink); },verified,true); return got == verified; }));
    }
    const auto keys = reader->physicalKeys(); Totals physical;
    push(measure("read_physical",s.name,dir,[&](FileIo &io) {
        tracked(*reader).io = {};
        readKeys([&](const auto &sink) { for (auto k : keys) sink(k); },physical,false); io = tracked(*reader).io;
        return QJsonObject{{"returnedTiles",decimal(physical.tiles)},{"returnedCells",decimal(physical.cells)},
            {"logicalZeroTilesSkipped",decimal(logicalTiles(s)-keys.size())}};
    },[&] { Totals verified;
        readKeys([&](const auto &sink) { for (auto k : keys) sink(k); },verified,true); return physical == verified; }));
    resourceCheckpoint(limits,dir,cancelled);
}
}
int main(int argc,char **argv) {
    qputenv("QT_COMMAND_LINE_PARSER_NO_GUI_MESSAGE_BOXES","1");
    QCoreApplication app(argc,argv); SetConsoleCtrlHandler(interrupt,TRUE);
    QCommandLineParser p; p.addHelpOption();
    p.setApplicationDescription("06.0B isolated count containers; generated 06.0A inputs; no production integration");
    p.addOptions({{"output-dir","New owned experiment directory","path"},
        {"scenario","all or a 06.0A scenario","name","all"},{"tile-size","Tile side, 16..1024, multiple of 16","cells","256"},
        {"grid-side","Default square size in tiles","n","3"},{"width","Override logical width","cells","0"},
        {"height","Override logical height","cells","0"},{"seed","Deterministic seed","n","6001"},
        {"repeats","Fresh-file, fresh-process trials per candidate (1..50)","n","4"},
        {"ram-mib","Cooperative RAM budget","MiB","256"},{"temp-mib","Temporary budget","MiB","128"},
        {"output-mib","Total retained output budget","MiB","1024"},{"storage-label","Media description","label"},
        {"candidate","Internal worker candidate D/E","name"},{"worker","Internal repetition index","n"}});
    p.process(app); QString owned; QJsonArray rows; QJsonObject report{{"status","error"},{"schema","density-formats-06.0B-v1"}};
    QElapsedTimer timer; timer.start();
    try {
        require(!p.value("output-dir").isEmpty(),"--output-dir is required");
        const auto dir = QFileInfo(p.value("output-dir")).absoluteFilePath();
        const auto side = option(p,"tile-size"), grid = option(p,"grid-side"), repeats = option(p,"repeats");
        require(side >= 16 && side <= 1024 && side%16 == 0 && repeats && repeats <= 50,"invalid tile/repeat options");
        const Limits limits{multiply(option(p,"ram-mib"),MiB),multiply(option(p,"temp-mib"),MiB),multiply(option(p,"output-mib"),MiB)};
        std::vector<Spec> specs;
        for (const auto &name : p.value("scenario") == "all" ? scenarioNames() : QStringList{p.value("scenario")}) {
            auto s = makeSpec(name,static_cast<std::uint32_t>(side),grid,option(p,"seed"));
            if (name != "huge-sparse" && (option(p,"width") || option(p,"height"))) {
                s.width = option(p,"width"); s.height = option(p,"height");
            }
            specFromJson(toJson(s)); specs.push_back(s);
        }
        require(!QFileInfo::exists(dir) && QDir().mkdir(dir),"output directory must be new and parent must exist"); owned = dir;
        report["environment"] = environment(DENSITY_SOURCE_DIR,dir,p.value("storage-label"));
        report["libraries"] = versions();
        if (p.isSet("worker")) {
            require(specs.size() == 1 && (p.value("candidate") == "D" || p.value("candidate") == "E"),"invalid worker arguments");
            report["spec"] = toJson(specs[0]);
            if (p.value("candidate") == "D") trial<DReader>(dir,specs[0],limits,rows);
            else trial<EReader>(dir,specs[0],limits,rows);
        } else {
            Plan demand{48*MiB,0,8*MiB};
            for (const auto &s : specs) for (bool tiff : {false,true}) {
                try { auto plan = storagePlan(s,tiff); demand.ramBytes = std::max(demand.ramBytes,plan.ramBytes);
                    demand.outputBytes = add(demand.outputBytes,multiply(repeats,plan.outputBytes)); }
                catch (const Failure &e) { if (e.status != "unsupported") throw; }
            }
            report["preflight"] = preflight(dir,limits,demand);
            report["parameters"] = QJsonObject{{"arguments",QJsonArray::fromStringList(app.arguments().mid(1))},
                {"order","per dataset/repetition: D,E on even trials; E,D on odd trials"},
                {"cachePolicy","uncontrolled; each trial writes a new file then reads it; first is not cold"},
                {"sink","timed reads sum counts and count tiles/cells; verification rereads every returned value outside timing"}};
            for (const auto &s : specs) for (U64 i = 0; i < repeats; ++i) {
                for (auto candidate : i%2 ? QStringList{"E","D"} : QStringList{"D","E"}) {
                    if (cancelled) throw Failure("cancelled","parent cancelled");
                    const auto workerDir = QDir(dir).filePath(QString("%1-%2-%3").arg(s.name,candidate,decimal(i)));
                    QStringList args{"--output-dir",workerDir,"--scenario",s.name,"--tile-size",decimal(side),
                        "--grid-side",decimal(grid),"--seed",decimal(s.seed),"--ram-mib",p.value("ram-mib"),
                        "--temp-mib",p.value("temp-mib"),"--output-mib",p.value("output-mib"),"--worker",decimal(i),"--candidate",candidate};
                    if (p.isSet("width") || p.isSet("height")) args << "--width" << decimal(s.width) << "--height" << decimal(s.height);
                    QProcess worker; worker.start(app.applicationFilePath(),args);
                    require(worker.waitForStarted(10000),"cannot start candidate worker");
                    while (!worker.waitForFinished(100)) if (cancelled) {
                        worker.kill(); worker.waitForFinished(5000); throw Failure("cancelled","parent cancelled worker"); }
                    auto result = load(QDir(workerDir).filePath("results.json"));
                    for (auto v : result["measurements"].toArray()) {
                        auto row = v.toObject(); row["candidate"] = candidate; row["repetition"] = int(i);
                        row["runClass"] = i ? "subsequent" : "first"; rows.append(row);
                    }
                    if (result["status"] == "unsupported") rows.append(QJsonObject{{"dataset",s.name},{"candidate",candidate},
                        {"phase","capability"},{"status","unsupported"},{"repetition",int(i)},{"diagnostic",result["diagnostic"]}});
                    else if (result["status"] != "ok" || worker.exitCode() || worker.exitStatus() != QProcess::NormalExit)
                        throw Failure("worker_error",QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
                    resourceCheckpoint(limits,dir,cancelled);
                }
            }
        }
        report["status"] = "ok";
    } catch (const Failure &e) { report["status"] = e.status; report["diagnostic"] = e.what(); }
      catch (const std::bad_alloc &) { report["status"] = "resource_refused"; report["diagnostic"] = "allocation failed"; }
      catch (const std::exception &e) { report["status"] = "error"; report["diagnostic"] = e.what(); }
    report["measurements"] = rows; report["subsequentStatistics"] = summary(rows);
    report["wholeOperationElapsedMs"] = double(timer.nsecsElapsed())/1000000.;
    try { if (!owned.isEmpty()) { save(QDir(owned).filePath("results.json"),report);
        FileIo io; writeNew(QDir(owned).filePath("results.csv"),csv(rows),io); } }
    catch (const std::exception &e) { std::fprintf(stderr,"report failure: %s\n",e.what()); return 1; }
    if (report["status"] != "ok") { std::fprintf(stderr,"%s: %s\n",qPrintable(report["status"].toString()),qPrintable(report["diagnostic"].toString())); return 1; }
    return 0;
}
