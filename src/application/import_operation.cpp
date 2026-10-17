#include "import_operation.h"

#include <QDir>
#include <QFileInfo>
#include <QUuid>

namespace cadcontour {
namespace {
WorkerOutcome failed(cache::Error error)
{
    auto failure = OperationFailure::make(FailureReason::ImportFailed, OperationPhase::Computing);
    failure.importError = std::move(error);
    return WorkerOutcome::failed(std::move(failure));
}
} // namespace

OperationWork ascXyzImportWork(ImportRequest request, QString storageRoot, io::ZHandler handler)
{
    return [request = std::move(request), root = std::move(storageRoot), handler = std::move(handler)]
        (const OperationSnapshot &, const WorkerControl &control) -> WorkerOutcome {
        if (control.cancelled())
            return WorkerOutcome::cancelled();
        control.report({OperationPhase::Computing, 0, std::nullopt});
        if (!QDir().mkpath(root))
            return failed({cache::ErrorCode::OpenFailed, cache::Phase::Publication,
                cache::Operation::Open, {}, 0, QStringLiteral("Cannot create import storage root"), {}});
        const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        auto directory = std::make_unique<QTemporaryDir>(root + QStringLiteral("/project-")
                                                         + id + QStringLiteral("-XXXXXX"));
        if (!directory->isValid() || !QDir(directory->path()).mkdir(QStringLiteral("datasets")))
            return failed({cache::ErrorCode::OpenFailed, cache::Phase::Publication,
                cache::Operation::Open, {}, 0, QStringLiteral("Cannot create import attempt directory"), {}});
        QTemporaryDir scratch(directory->path() + QStringLiteral("/scratch-XXXXXX"));
        if (!scratch.isValid())
            return failed({cache::ErrorCode::OpenFailed, cache::Phase::Ingest,
                cache::Operation::Open, {}, 0, QStringLiteral("Cannot create import scratch directory"), {}});
        const auto relative = QStringLiteral("datasets/")
            + QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral(".pc2dcache");
        auto result = io::importAscXyz(request.sourcePath, QDir(directory->path()).filePath(relative),
            scratch.path(), request.limits, handler,
            [](void *context) noexcept { return static_cast<const WorkerControl *>(context)->cancelled(); },
            const_cast<WorkerControl *>(&control));
        if (!result) {
            if (result.error().code == cache::ErrorCode::Cancelled)
                return WorkerOutcome::cancelled();
            return failed(result.error());
        }
        ImportReadyProduct ready{result.value().cache.metadata(), result.value().source,
            request.sourcePath, id, directory->path(), relative};
        control.report({OperationPhase::Finalizing, ready.metadata.pointCount, ready.metadata.pointCount});
        return WorkerOutcome::success(std::make_unique<WorkerImportProduct>(
            WorkerImportProduct{std::move(directory), std::move(result.value()), std::move(ready)}));
    };
}

} // namespace cadcontour
