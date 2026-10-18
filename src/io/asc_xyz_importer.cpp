#include "asc_xyz_importer.h"

#include <QDateTime>
#include <QFileInfo>

#include <new>

namespace cadcontour::io {
namespace {
using namespace cache;
Error error(ErrorCode code, const char *detail)
{
    return makeError(code, Phase::SourceRead, Operation::Read, QString::fromLatin1(detail));
}
Status checkpoint(CancelCheck cancel, void *context)
{
    return cancel && cancel(context) ? Status{error(ErrorCode::Cancelled, "ASC/XYZ import cancelled")} : success();
}
Result<ImportProduct> run(const QString &sourcePath, const QString &cachePath, const QString &parent,
                          ResourceLimits limits, const ZHandler &handler, CancelCheck cancel, void *context,
                          const ImportObserver &observer)
{
    if (auto s = checkpoint(cancel, context); !s) return s.error();
    const auto paths = checked::multiply(static_cast<std::uint64_t>(sourcePath.size()), 16);
    const auto reserve = paths ? checked::add(ascParserReserve, *paths) : std::nullopt;
    if (!reserve || limits.memoryBytes <= *reserve || limits.maxOpenFiles <= 1)
        return error(ErrorCode::ResourceExhausted, "Insufficient parser/source budget");
    limits.memoryBytes -= *reserve;
    --limits.maxOpenFiles;
    QFile source(sourcePath);
    if (!source.open(QIODevice::ReadOnly | QIODevice::Unbuffered))
        return makeError(ErrorCode::OpenFailed, Phase::SourceRead, Operation::Open,
                         source.errorString(), {}, source.error());
    auto initialSize = file::size(source);
    if (!initialSize) return initialSize.error();
    const auto modified = QFileInfo(source).lastModified();
    auto created = CacheWriter::create(cachePath, limits, parent, cancel, context);
    if (!created) return created.error();
    auto writer = std::move(created.value());
    if (observer) observer(ImportPhase::Parsing);
    auto parsed = parseAscXyz(source, [&](auto batch) { return writer->append(batch); }, handler, cancel, context);
    if (!parsed) return parsed.error();
    auto finalSize = file::size(source);
    if (!finalSize) return finalSize.error();
    if (finalSize.value() != initialSize.value() || parsed.value().bytesRead != initialSize.value()
        || QFileInfo(source).lastModified() != modified)
        return error(ErrorCode::SourceChanged, "Source size or modification time changed during import");
    source.close();
    if (auto s = checkpoint(cancel, context); !s) return s.error();
    if (observer) observer(ImportPhase::BuildingCache);
    auto finished = writer->finish();
    if (!finished) return finished.error();
    if (auto s = checkpoint(cancel, context); !s) return s.error();
    return ImportProduct{std::move(finished.value()), parsed.value()};
}
} // namespace
cache::Result<ImportProduct> importAscXyz(const QString &sourcePath, const QString &cachePath,
    const QString &parent, cache::ResourceLimits limits, const ZHandler &handler, CancelCheck cancel, void *context,
    const ImportObserver &observer)
{
    try { return run(sourcePath, cachePath, parent, limits, handler, cancel, context, observer); }
    catch (const std::bad_alloc &) {
        return cache::Error{cache::ErrorCode::ResourceExhausted, cache::Phase::SourceRead,
            cache::Operation::Read, {}, 0, QStringLiteral("Importer allocation failed"), {}};
    }
}
} // namespace cadcontour::io
