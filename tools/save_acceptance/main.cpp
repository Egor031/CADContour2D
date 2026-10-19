#include "../import_acceptance/measurement.h"
#include "operation_test_access.h"
#include "save_test_access.h"
#include "cache/cache_writer.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <array>
#include <cstdio>
#include <cstring>

using namespace cadcontour;

namespace {
void finishSave(ProjectController &c)
{
    QElapsedTimer deadline; deadline.start();
    while (c.operation().active && deadline.elapsed() < 600000) {
        OperationTestAccess::poll(c);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
        QThread::yieldCurrentThread();
    }
    require(!c.operation().active && c.operation().status == OperationStatus::Succeeded, "Save did not succeed");
}
}
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("CADContour2D-save-acceptance"));
    QCommandLineParser args;
    args.setApplicationDescription("Opt-in synthetic Save resource comparison; never production CTest.");
    args.addHelpOption();
    args.addOptions({{"points", "Synthetic point count (1..100000000).", "count"},
                     {"root", "New isolated workload directory; must not exist.", "path"},
                     {"report", "New JSON report path; creates two sample CSV files.", "path"}});
    if (!args.parse(app.arguments())) {
        std::fprintf(stderr, "%s\n", args.errorText().toLocal8Bit().constData());
        return 2;
    }
    if (args.isSet("help")) {
        const auto help = args.helpText().toUtf8();
        std::fwrite(help.data(), 1, std::size_t(help.size()), stdout);
        return 0;
    }
    try {
        bool ok = false; const auto count = args.value("points").toULongLong(&ok);
        require(ok && count && count <= 100000000, "Invalid --points");
        const auto root = QFileInfo(args.value("root")).absoluteFilePath();
        const auto report = QFileInfo(args.value("report")).absoluteFilePath();
        require(!args.value("root").isEmpty() && !args.value("report").isEmpty(), "--root and --report required");
        require(!QFileInfo::exists(root) && QDir().mkpath(root), "Workload directory must be new");
        QFile output(report); require(output.open(QIODevice::WriteOnly | QIODevice::NewOnly), "Report must be new");
        const auto cachePath = QDir(root).filePath(QStringLiteral("source.pc2dcache"));
        auto writer = cache::CacheWriter::create(cachePath, {16ULL << 20, 16ULL << 30, 32}, root);
        require(bool(writer), "Cannot create fixture writer");
        std::array<cache::Measurement, 4096> batch;
        for (std::uint64_t first = 0; first < count;) {
            const auto n = std::min<std::uint64_t>(batch.size(), count - first);
            for (std::uint64_t i = 0; i < n; ++i) {
                const auto id = first + i;
                batch[i] = {double(id % 100000) * .01, double(id / 100000) * .01, 0};
            }
            require(bool(writer.value()->append({batch.data(), std::size_t(n)})), "Cannot append synthetic fixture");
            first += n;
        }
        auto pending = writer.value()->finish(); require(bool(pending), "Cannot complete synthetic fixture");
        const auto metadata = pending.value().metadata(); pending.value().releaseToOwner(); writer.value().reset();
        ImportedDataset dataset;
        dataset.cacheId = metadata.id;
        dataset.location = std::make_shared<const DatasetLocation>(DatasetLocation{
            QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString(), root.toStdWString(), L"source.pc2dcache"});
        dataset.sourcePath = L"synthetic-fixture.xyz"; dataset.sourceLines = count; dataset.sourceBytes = count * 40;
        dataset.pointCount = count; dataset.minimum = {metadata.bounds.minX, metadata.bounds.minY};
        dataset.maximum = {metadata.bounds.maxX, metadata.bounds.maxY};
        ProjectId id; const auto uuid = QUuid::createUuid().toRfc4122(); std::memcpy(id.bytes.data(), uuid.data(), 16);
        ProjectController controller(root + QStringLiteral("/unused-import"));
        SaveTestAccess::installFixture(controller, ProjectState(1, id, std::move(dataset)));
        std::atomic<std::uint64_t> copies{0};
        SaveTestAccess::configure(controller, [&](persistence::SavePhase phase) {
            if (phase == persistence::SavePhase::DatasetPublished) ++copies;
        });
        QJsonObject result{{"synthetic", true}, {"root", root},
            {"notes", "Logical I/O and sampled EOF, warm OS caches; process lifetime peaks include fixture creation. 16 MiB writer budget, 64 KiB save copy buffer."}};
        number(result, "pointCount", count); number(result, "cacheBytes", QFileInfo(cachePath).size());
        for (const bool repeat : {false, true}) {
            if (repeat) controller.setCell(.5);
            const auto prefix = repeat ? QStringLiteral("repeat") : QStringLiteral("first");
            const auto before = copies.load();
            Measurement measurement(root, report + u'.' + prefix + QStringLiteral(".csv"));
            const auto start = std::chrono::steady_clock::now();
            const auto command = repeat ? controller.saveProject() : controller.saveProject({root + QStringLiteral("/project/P.pc2dproj")});
            require(command.outcome == CommandOutcome::Applied, "Save command rejected"); finishSave(controller);
            const auto elapsed = since(start);
            auto item = measurement.json(); item["elapsedMs"] = elapsed;
            number(item, "datasetCopies", copies.load() - before);
            std::uint64_t finalBytes = 0;
            for (const auto &path : filesIn(root + QStringLiteral("/project"))) finalBytes += QFileInfo(path).size();
            number(item, "finalProjectEofBytes", finalBytes);
            result[prefix] = item;
        }
        const auto bytes = QJsonDocument(result).toJson();
        require(output.write(bytes) == bytes.size() && output.flush(), "Cannot write report");
        std::fwrite(bytes.data(), 1, std::size_t(bytes.size()), stdout);
        return 0;
    } catch (const std::exception &e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
}
