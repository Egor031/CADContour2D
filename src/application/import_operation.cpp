#include "import_operation.h"

#include <QDir>
#include <QFileInfo>
#include <QStorageInfo>
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

cache::Result<cache::ResourceLimits> interactiveImportLimits(std::uint64_t availableBytes)
{
    constexpr std::uint64_t reserve = 64ULL << 20;
    if (availableBytes <= reserve)
        return cache::makeError(cache::ErrorCode::ResourceExhausted, cache::Phase::Ingest,
            cache::Operation::Size, QStringLiteral("Insufficient available storage for interactive import"));
    const auto usable = availableBytes - reserve;
    return cache::ResourceLimits{128ULL << 20, usable - usable / 20, 32};
}

OperationWork ascXyzImportWork(ImportRequest request, QString storageRoot, io::ZHandler handler)
{
    return [request = std::move(request), root = std::move(storageRoot), handler = std::move(handler)]
        (const OperationSnapshot &, const WorkerControl &control) -> WorkerOutcome {
        if (control.cancelled())
            return WorkerOutcome::cancelled();
        control.report({OperationPhase::Preparing, 0, std::nullopt});
        if (!QDir().mkpath(root))
            return failed({cache::ErrorCode::OpenFailed, cache::Phase::Publication,
                cache::Operation::Open, {}, 0, QStringLiteral("Cannot create import storage root"), {}});
        auto limits = request.limits;
        if (request.interactiveResources) {
            const QStorageInfo storage(root);
            if (!storage.isValid() || !storage.isReady() || storage.bytesAvailable() < 0)
                return failed(cache::makeError(cache::ErrorCode::ResourceExhausted, cache::Phase::Ingest,
                    cache::Operation::Size, QStringLiteral("Cannot determine available import storage")));
            auto profile = interactiveImportLimits(static_cast<std::uint64_t>(storage.bytesAvailable()));
            if (!profile) return failed(profile.error());
            limits = profile.value();
        }
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
        // Without a registered consumer keep the parser's explicit fail-closed
        // diagnostic. The synchronous handler only borrows this live control.
        io::ZHandler confirmation = handler;
        if (!confirmation && control.nonZeroZConfirmationAvailable())
            confirmation = [&control](io::NonZeroZ z) { return control.confirmNonZeroZ(z); };
        auto result = io::importAscXyz(request.sourcePath, QDir(directory->path()).filePath(relative),
            scratch.path(), limits, confirmation,
            [](void *context) noexcept { return static_cast<const WorkerControl *>(context)->cancelled(); },
            const_cast<WorkerControl *>(&control), [&control](io::ImportPhase phase) {
                control.report({phase == io::ImportPhase::Parsing ? OperationPhase::Parsing
                    : OperationPhase::BuildingCache, 0, std::nullopt});
            });
        if (!result) {
            if (result.error().code == cache::ErrorCode::Cancelled)
                return WorkerOutcome::cancelled();
            return failed(result.error());
        }
        ImportReadyProduct ready{result.value().cache.metadata(), result.value().source,
            request.sourcePath, id, directory->path(), relative};
        control.report({OperationPhase::Finalizing, 0, std::nullopt});
        return WorkerOutcome::success(std::make_unique<WorkerImportProduct>(
            WorkerImportProduct{std::move(directory), std::move(result.value()), std::move(ready)}));
    };
}

} // namespace cadcontour
