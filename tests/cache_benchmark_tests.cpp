#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

namespace {
class CacheBenchmark : public testing::Test {
protected:
    QTemporaryDir root;
    struct Run {
        int exitCode = -1;
        QByteArray output, error;
        QJsonObject report;
    };
    Run run(const QStringList &options)
    {
        EXPECT_TRUE(root.isValid());
        const auto parent = root.filePath("data"), scratch = root.filePath("scratch");
        EXPECT_TRUE(QDir().mkdir(parent));
        EXPECT_TRUE(QDir().mkdir(scratch));
        const auto reportPath = root.filePath("report.json");
        QProcess process;
        process.setProgram(QString::fromUtf8(CACHE_BENCHMARK_EXE));
        process.setArguments(QStringList{"--parent", parent, "--temp-parent", scratch,
            "--report", reportPath, "--memory-mib", "8", "--repeats", "2"} + options);
        process.start();
        EXPECT_TRUE(process.waitForStarted(10000));
        const auto finished = process.waitForFinished(10000);
        EXPECT_TRUE(finished) << "benchmark did not reject/complete bounded input";
        if (!finished) { process.kill(); process.waitForFinished(10000); }
        EXPECT_EQ(process.exitStatus(), QProcess::NormalExit);
        Run result{process.exitCode(), process.readAllStandardOutput(), process.readAllStandardError(), {}};
        QFile file(reportPath);
        EXPECT_TRUE(file.open(QIODevice::ReadOnly));
        QJsonParseError error{};
        const auto document = QJsonDocument::fromJson(file.readAll(), &error);
        EXPECT_EQ(error.error, QJsonParseError::NoError);
        EXPECT_TRUE(document.isObject());
        result.report = document.object();
        // Early failures have no dataset to preserve; successful runs also clean both roots.
        for (const auto &path : {parent, scratch})
            EXPECT_TRUE(QDir(path).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden).isEmpty());
        return result;
    }
    void rejected(const QStringList &options, const QString &diagnostic)
    {
        const auto result = run(options);
        EXPECT_EQ(result.exitCode, 1) << result.error.constData();
        EXPECT_EQ(result.report["status"], "FAIL");
        EXPECT_TRUE(result.report["error"].toString().contains(diagnostic)) << result.error.constData();
        EXPECT_TRUE(QString::fromUtf8(result.error).contains(diagnostic));
        EXPECT_FALSE(result.report.contains("buildMs"));
        EXPECT_FALSE(result.report.contains("buildResources"));
        EXPECT_FALSE(result.report.contains("mortonQueries"));
        EXPECT_FALSE(result.report.contains("retainedDataDirectory"));
        EXPECT_FALSE(result.report.contains("retainedScratchDirectory"));
        EXPECT_FALSE(result.output.contains("built"));
    }
};
TEST_F(CacheBenchmark, RejectsMaximumCountBeforeOracle)
{
    rejected({"--points", "18446744073709551615"}, "File layout exceeds representable offsets");
}
TEST_F(CacheBenchmark, RejectsDiskBudgetBeforeOracle)
{
    // Layout fits qint64 and Writer can initialize with 1 MiB of disk, but no runs fit.
    rejected({"--points", "1000000000000", "--disk-mib", "1"}, "predicted dataset exceeds split disk budget");
}
TEST_F(CacheBenchmark, RejectsQint64LayoutWithoutUint64Overflow)
{
    constexpr std::uint64_t records = 400000000000000000ULL;
    static_assert(records <= std::numeric_limits<std::uint64_t>::max() / 24);
    static_assert(records * 24 > std::uint64_t(std::numeric_limits<qint64>::max()));
    rejected({"--points", QString::number(records)}, "File layout exceeds representable offsets");
}
TEST_F(CacheBenchmark, RejectsGenerationSizeOverflow)
{
    // Point layout fits qint64, but two 40-byte generations exceed uint64.
    rejected({"--points", "250000000000000000"}, "predicted dataset exceeds split disk budget");
}
TEST_F(CacheBenchmark, RejectsZeroCount)
{
    rejected({"--points", "0"}, "invalid size or budget");
}
TEST_F(CacheBenchmark, RejectsZeroMemoryBeforeOracle)
{
    rejected({"--points", "1000000000000", "--memory-mib", "0"}, "Zero resource budget");
}
TEST_F(CacheBenchmark, SmallRunPreservesReportAndQueryContracts)
{
    const auto result = run({"--points", "12000"});
    ASSERT_EQ(result.exitCode, 0) << result.error.constData();
    ASSERT_EQ(result.report["status"], "PASS");
    EXPECT_TRUE(result.error.isEmpty()) << result.error.constData();
    QFile reference(QString::fromUtf8(CACHE_BENCHMARK_REFERENCE));
    ASSERT_TRUE(reference.open(QIODevice::ReadOnly));
    const auto old = QJsonDocument::fromJson(reference.readAll()).object();
    EXPECT_EQ(result.report.keys(), old.keys());
    EXPECT_EQ(result.report["pointCount"], "12000");
    EXPECT_EQ(result.report["pointRecordBytes"], "288000");
    double phaseMs = 0;
    const auto phases = result.report["phaseMs"].toObject();
    for (auto it = phases.begin(); it != phases.end(); ++it) phaseMs += it.value().toDouble();
    EXPECT_GT(result.report["buildMs"].toDouble(), 0);
    // Observer also includes monitor setup and final bookkeeping around the build timer.
    EXPECT_GE(phaseMs, result.report["buildMs"].toDouble());
    const auto resources = result.report["buildResources"].toObject();
    EXPECT_GT(resources["processLifetimePeakWorkingSetBytes"].toString().toULongLong(), 0);
    EXPECT_GT(resources["processWriteTransferBytes"].toString().toULongLong(), 0);
    EXPECT_GT(result.report["observedPeakWorkingArrayBytes"].toString().toULongLong(), 0);
    EXPECT_LT(result.report["observedPeakWorkingArrayBytes"].toString().toULongLong(), 8ULL * 1024 * 1024);
    EXPECT_LE(result.report["observedPeakDatasetFileHandles"].toString().toUInt(), 4);
    for (const auto *name : {"mortonQueries", "baselineQueries"}) {
        const auto queries = result.report[name].toArray();
        ASSERT_EQ(queries.size(), 14); // Seven classes, two timed passes; exact oracle is separate.
        for (const auto &entry : queries) {
            const auto query = entry.toObject();
            EXPECT_EQ(query.keys(), old[name].toArray()[0].toObject().keys());
            if (query["query"] == "scanAll") EXPECT_EQ(query["returnedRecords"], "12000");
        }
    }
    EXPECT_EQ(result.report["validation"].toObject()["returnedRecords"], "12000");
}
}
