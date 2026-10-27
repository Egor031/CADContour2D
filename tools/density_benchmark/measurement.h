#pragma once
#include "dataset.h"
#include "cache/cache_reader.h"

#include <QJsonArray>

namespace density {
struct Limits { U64 ramBytes = 128 * MiB, temporaryBytes = 128 * MiB, outputBytes = 128 * MiB; };
struct Plan { U64 ramBytes = 0, temporaryBytes = 0, outputBytes = 0; };
QJsonObject preflight(const QString &directory, Limits limits, Plan plan);
QJsonObject environment(const QString &sourceDirectory, const QString &outputDirectory, const QString &storageLabel);
QJsonObject cacheStatistics(const cadcontour::cache::ReadStatistics &stats);
QJsonObject ioJson(const FileIo &io);
QByteArray csv(const QJsonArray &rows);
QJsonObject summary(const QJsonArray &rows);
U64 directoryBytes(const QString &directory);

// One callback = one measured operation. The validation callback runs afterwards
// and has its own elapsed time. No adapter/plugin registry or candidate exists in 06.0A.
// 06.0B passes its direct write/read call here; 06.0C passes its raster builder.
QJsonObject measure(const QString &phase, const QString &dataset, const QString &ownedDirectory,
                    const std::function<QJsonObject(FileIo &)> &operation,
                    const std::function<bool()> &verify = {});
U64 workingSet();
void resourceCheckpoint(Limits limits, const QString &ownedDirectory, bool cancelled);
} // namespace density
