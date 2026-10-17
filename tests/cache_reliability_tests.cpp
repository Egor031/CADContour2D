#include "cache/cache_reader.h"
#include "cache/cache_writer.h"

#include <QCryptographicHash>
#include <QDirIterator>
#include <QFileInfo>
#include <QProcess>
#include <gtest/gtest.h>

using namespace cadcontour::cache;

namespace {
QByteArray digest(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) return {};
    return hash.result();
}
class CacheCrash : public testing::TestWithParam<const char *> {};
TEST_P(CacheCrash, PreviousDatasetAndForeignFilesSurviveAbruptChildExit)
{
    QTemporaryDir dir; ASSERT_TRUE(dir.isValid());
    const auto oldPath = dir.filePath("previous.cache"), target = dir.filePath("new.cache");
    const ResourceLimits limits{1024 * 1024, 16 * 1024 * 1024, 4};
    {
        auto w = CacheWriter::create(oldPath, limits, dir.path()); ASSERT_TRUE(w);
        const Measurement m{-0., 3., 0.}; ASSERT_TRUE(w.value()->append({&m, 1}));
        auto p = w.value()->finish(); ASSERT_TRUE(p);
        ASSERT_EQ(p.value().releaseToOwner(), oldPath);
    }
    const auto oldHash = digest(oldPath); ASSERT_FALSE(oldHash.isEmpty());
    const auto foreignPath = dir.filePath("unrelated.txt");
    { QFile f(foreignPath); ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::NewOnly));
      ASSERT_EQ(f.write("foreign sentinel"), 16); }
    const auto foreignHash = digest(foreignPath);
    QProcess child;
    child.setProgram(QString::fromUtf8(CACHE_CRASH_PROBE_EXE));
    child.setArguments({QString::fromLatin1(GetParam()), target, dir.path()});
    child.start(); ASSERT_TRUE(child.waitForStarted(10000));
    QByteArray output;
    while (!output.contains("READY\n") && !output.contains("READY\r\n")) {
        if (!child.waitForReadyRead(10000)) break;
        output += child.readAllStandardOutput();
    }
    ASSERT_TRUE(output.contains("READY")) << output.constData() << child.readAllStandardError().constData();
    const bool published = QString::fromLatin1(GetParam()).startsWith("after-");
    EXPECT_EQ(QFileInfo::exists(target), published);
    child.kill(); ASSERT_TRUE(child.waitForFinished(10000));
    EXPECT_EQ(child.exitStatus(), QProcess::CrashExit);
    EXPECT_EQ(digest(oldPath), oldHash);
    EXPECT_EQ(digest(foreignPath), foreignHash);
    auto previous = CacheReader::open(oldPath, limits); ASSERT_TRUE(previous);
    EXPECT_TRUE(previous.value()->validate(limits, dir.path()));
    int leftovers = 0;
    QDirIterator files(dir.path(), QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    while (files.hasNext()) {
        const auto path = files.next();
        if (path == oldPath || path == foreignPath) continue;
        ++leftovers;
        if (QFileInfo(path).fileName().startsWith(".pc2d-") || path == target) {
            auto opened = CacheReader::open(path, limits);
            if (QString::fromLatin1(GetParam()) == "before-seal") EXPECT_FALSE(opened);
            else { ASSERT_TRUE(opened); EXPECT_TRUE(opened.value()->validate(limits, dir.path())); }
        }
    }
    EXPECT_GT(leftovers, 0); // No scavenger: crash leftovers belong to this isolated test directory.
    EXPECT_EQ(QFileInfo::exists(target), published);
}
INSTANTIATE_TEST_SUITE_P(LifecycleCheckpoints, CacheCrash,
    testing::Values("before-seal", "before-rename", "after-rename", "after-release"));
}
