#include "density_operation.h"

#include <QDir>

#include <bit>
#include <exception>
#include <new>
#include <type_traits>
#include <utility>

namespace cadcontour {
namespace {
using density::BuildError;
using density::BuildErrorCode;
using density::BuildPhase;
using density::U64;

// Late diagnostics use only implicit shared-string copies and scalar optionals;
// retaining the first bounded diagnostic cannot allocate in a noexcept bridge.
static_assert(std::is_nothrow_copy_assignable_v<std::optional<BuildError>>);
static_assert(std::is_nothrow_copy_constructible_v<BuildError>);

bool same(double a, double b) noexcept
{ return std::bit_cast<U64>(a) == std::bit_cast<U64>(b); }
bool same(ModelPoint a, ModelPoint b) noexcept
{ return same(a.x, b.x) && same(a.y, b.y); }
bool same(const raster::Mapping &a, const raster::Mapping &b) noexcept
{
    return same(a.minimum(), b.minimum()) && same(a.maximum(), b.maximum())
        && same(a.cell(), b.cell()) && a.width() == b.width() && a.height() == b.height()
        && a.convention() == b.convention();
}

bool separator(QChar c) noexcept { return c == u'/' || c == u'\\'; }
bool qualifiedDrive(QStringView p) noexcept
{
    if (p.size() < 3) return false;
    const auto d = p[0].unicode();
    return ((d >= u'A' && d <= u'Z') || (d >= u'a' && d <= u'z'))
        && p[1] == u':' && separator(p[2]);
}
bool qualifiedUnc(QStringView p) noexcept
{
    qsizetype serverEnd = 0;
    while (serverEnd < p.size() && !separator(p[serverEnd])) ++serverEnd;
    if (!serverEnd || serverEnd == p.size()) return false;
    const auto server = p.first(serverEnd);
    if (server == u"." || server == u".." || server == u"?") return false;
    const auto shareStart = serverEnd + 1;
    auto shareEnd = shareStart;
    while (shareEnd < p.size() && !separator(p[shareEnd])) ++shareEnd;
    const auto share = p.sliced(shareStart, shareEnd - shareStart);
    return !share.isEmpty() && share != u"." && share != u"..";
}
// The adapter must reject CWD-dependent syntax before creating its directories;
// the builder independently enforces the same source/output/scratch contract.
bool fullyQualified(QStringView p) noexcept
{
    if (p.contains(QChar(0))) return false;
    if (p.size() >= 4 && p[0] == u'\\' && p[1] == u'\\'
        && (p[2] == u'?' || p[2] == u'.') && p[3] == u'\\') {
        const auto tail = p.sliced(4);
        if (tail.contains(u'/')) return false;
        if (qualifiedDrive(tail)) return true;
        return tail.startsWith(u"UNC\\", Qt::CaseInsensitive) && qualifiedUnc(tail.sliced(4));
    }
    return qualifiedDrive(p) || (p.size() >= 2 && separator(p[0])
        && separator(p[1]) && qualifiedUnc(p.sliced(2)));
}
bool completedPathMatches(const QString &scope, const QString &path) noexcept
{
    constexpr auto suffix = u"/counts.pc2ddensity";
    const QStringView prefix(scope), completed(path), tail(suffix);
    return !prefix.isEmpty() && !separator(prefix.back())
        && completed.size() == prefix.size() + tail.size()
        && completed.startsWith(prefix) && completed.sliced(prefix.size()) == tail;
}
bool same(const density::BuildStatistics &a, const density::BuildStatistics &b) noexcept
{
#define SAME(field) if (a.field != b.field) return false
    SAME(scanned); SAME(eventsWritten); SAME(eventsRead); SAME(accumulated); SAME(tileTotal);
    SAME(scanAllCalls); SAME(spatialQueries); SAME(occupiedGroups); SAME(visitedGroups); SAME(tiles);
    SAME(eventBytesWritten); SAME(eventBytesRead); SAME(eventWrites); SAME(eventReads); SAME(eventOpens);
    SAME(workingPeak); SAME(metadataPeak); SAME(scratchPeak); SAME(combinedDiskPeak); SAME(filesPeak);
    SAME(handlesPeak); SAME(workUnits); SAME(mappingNanoseconds); SAME(sourceOpenNanoseconds);
    SAME(scanNanoseconds); SAME(groupingNanoseconds); SAME(eventSortNanoseconds); SAME(eventIONanoseconds);
    SAME(accumulationNanoseconds); SAME(tileCopyNanoseconds); SAME(densityIngestNanoseconds); SAME(finishNanoseconds);
    SAME(density.physicalTiles); SAME(density.submissions); SAME(density.total); SAME(density.readBytes);
    SAME(density.writtenBytes); SAME(density.decodedTiles); SAME(density.indexReads);
    SAME(density.readOperations); SAME(density.writeOperations); SAME(density.openOperations);
    SAME(density.workUnits); SAME(density.bufferReserve); SAME(density.codecPeak); SAME(density.sortReserve);
    SAME(density.scratchPeak); SAME(density.combinedDiskPeak); SAME(density.outputBytes); SAME(density.mergePasses);
    SAME(density.mappingNanoseconds); SAME(density.sortNanoseconds); SAME(density.copyNanoseconds);
    SAME(density.readbackNanoseconds);
    SAME(sourceRead.indexPages); SAME(sourceRead.pointPages); SAME(sourceRead.readBytes);
    SAME(sourceRead.readOperations); SAME(sourceRead.seekOperations); SAME(sourceRead.returnedRecords);
    SAME(sourceRead.bufferBytes); SAME(sourceRead.traversalFrames);
#undef SAME
    return true;
}
OperationPhase operationPhase(BuildPhase phase) noexcept
{
    switch (phase) {
    case BuildPhase::Scan: return OperationPhase::Scan;
    case BuildPhase::Grouping: return OperationPhase::Grouping;
    case BuildPhase::Accumulation:
    case BuildPhase::TileCopy:
    case BuildPhase::DensityIngest: return OperationPhase::Accumulation;
    case BuildPhase::Finish:
    case BuildPhase::Complete: return OperationPhase::Finalizing;
    case BuildPhase::Cleanup: return OperationPhase::Cleanup;
    default: return OperationPhase::Preparing;
    }
}
WorkerOutcome failed(BuildError error)
{
    auto failure = OperationFailure::make(FailureReason::DensityFailed, operationPhase(error.phase));
    if (error.code == BuildErrorCode::InvalidInput || error.code == BuildErrorCode::EmptySource)
        failure.category = FailureCategory::InvalidInput;
    if (error.code == BuildErrorCode::ResourceRefused)
        failure.category = FailureCategory::ResourceUnavailable;
    failure.densityError = std::move(error);
    return WorkerOutcome::failed(std::move(failure));
}
void cleanupError(DensityOperationDiagnostics &diagnostics) noexcept
{
    if (!diagnostics.cleanupError)
        diagnostics.cleanupError = BuildError{BuildErrorCode::CleanupFailure, BuildPhase::Cleanup,
            QStringLiteral("Cannot remove owned Density attempt directory")};
}
struct Bridge {
    const WorkerControl &control;
    U64 sourceCount;
    std::shared_ptr<DensityOperationDiagnostics> diagnostics;
    const std::function<void(BuildPhase)> *hook;
    static bool cancel(void *context) noexcept
    {
        const auto &self = *static_cast<Bridge *>(context);
        return self.diagnostics->bridgeFailed || self.control.cancelled();
    }
    static void progress(const density::BuildProgress &p, void *context) noexcept
    {
        auto &self = *static_cast<Bridge *>(context);
        if (self.diagnostics->bridgeFailed) return;
        try {
            OperationProgress progress{operationPhase(p.phase), 0, std::nullopt};
            switch (p.phase) {
            case BuildPhase::Scan: progress.completed = p.scanned; progress.total = self.sourceCount; break;
            case BuildPhase::Grouping: progress.completed = p.groups; break;
            case BuildPhase::Accumulation:
            case BuildPhase::TileCopy:
            case BuildPhase::DensityIngest: progress.completed = p.eventsRead; progress.total = self.sourceCount; break;
            case BuildPhase::Finish:
            case BuildPhase::Complete: progress.completed = p.tiles; break;
            default: break;
            }
            self.control.report(progress);
            if (self.hook && *self.hook) (*self.hook)(p.phase);
        } catch (...) {
            self.diagnostics->bridgePhase = p.phase;
            self.diagnostics->bridgeFailed = true;
        }
    }
    static void diagnostic(const BuildError &error, void *context) noexcept
    {
        auto &diagnostics = *static_cast<DensityOperationDiagnostics *>(context);
        if (!diagnostics.cleanupError) diagnostics.cleanupError = error;
    }
};
} // namespace

bool validDensityReady(const DensityBuildSnapshot &snapshot, const DensityReadyProduct &ready) noexcept
{
    const auto &m = ready.metadata;
    const auto &s = ready.statistics;
    return ready.snapshot.get() == &snapshot && !snapshot.projectId.isNull()
        && snapshot.source.location && !snapshot.source.cacheId.isNull() && snapshot.source.pointCount && !m.id.isNull()
        && m.source == snapshot.source.cacheId && m.sourceMeasurements == snapshot.source.pointCount
        && m.contract == density::BuildContract::CompletePointCache && m.tileSide == snapshot.tileSide
        && same(m.mapping.minimum(), snapshot.source.minimum)
        && same(m.mapping.maximum(), snapshot.source.maximum) && same(m.mapping.cell(), snapshot.cell)
        && m.mapping.convention() == raster::Convention::Binary64SeparateRoundNearestEven
        && m.mapping.width() && m.mapping.height()
        && completedPathMatches(snapshot.outputScope, ready.completedPath)
        && s.scanned == snapshot.source.pointCount && s.eventsWritten == s.scanned
        && s.eventsRead == s.scanned && s.accumulated == s.scanned && s.tileTotal == s.scanned
        && s.scanAllCalls == 1 && s.spatialQueries == 0
        && s.occupiedGroups && s.occupiedGroups == s.visitedGroups && s.tiles
        && s.density.total == s.scanned && s.density.physicalTiles == s.tiles
        && s.workingPeak <= snapshot.resources.workingBytes
        && s.metadataPeak <= snapshot.resources.metadataBytes && s.scratchPeak <= snapshot.resources.scratchBytes
        && s.combinedDiskPeak <= snapshot.resources.combinedDiskBytes && s.filesPeak <= snapshot.resources.files
        && s.handlesPeak <= snapshot.resources.handles && s.workUnits <= snapshot.resources.workUnits
        && s.density.outputBytes <= snapshot.resources.outputBytes;
}

DensityAttemptDirectory::DensityAttemptDirectory(QString path,
        std::shared_ptr<DensityOperationDiagnostics> diagnostics)
    : path_(std::move(path)), scratch_(QDir(path_).filePath(QStringLiteral("scratch"))),
      diagnostics_(std::move(diagnostics)) {}
bool DensityAttemptDirectory::create()
{
    if (created_ || !QDir().mkdir(path_)) return false;
    created_ = true;
    scratchCreated_ = QDir().mkdir(scratch_);
    return scratchCreated_;
}
DensityAttemptDirectory::~DensityAttemptDirectory()
{
    try {
        if (scratchCreated_ && !QDir().rmdir(scratch_)) cleanupError(*diagnostics_);
        if (created_ && !retained_ && !QDir().rmdir(path_)) cleanupError(*diagnostics_);
    } catch (...) { cleanupError(*diagnostics_); }
}
bool WorkerDensityProduct::valid(const DensityBuildSnapshot &snapshot) const noexcept
{
    if (!directory || !directory->created() || directory->retained() || !output || !ready || !diagnostics
        || directory->path() != snapshot.outputScope || !validDensityReady(snapshot, *ready)) return false;
    const auto &actual = output->dataset.metadata(), &m = ready->metadata;
    return output->dataset.path() == ready->completedPath && actual.id == m.id
        && actual.source == m.source && actual.sourceMeasurements == m.sourceMeasurements
        && actual.contract == m.contract && actual.tileSide == m.tileSide
        && same(actual.mapping, m.mapping) && same(output->statistics, ready->statistics);
}
void WorkerDensityProduct::accept() noexcept
{
    directory->retain();
    output->dataset.releaseToOwner();
}
bool WorkerDensityProduct::cleanup() noexcept
{
    output.reset();
    directory.reset();
    return diagnostics && diagnostics->cleanupError.has_value();
}

OperationWork DensityOperation::work(std::shared_ptr<const DensityBuildSnapshot> snapshot)
{ return work(std::move(snapshot), {}); }
OperationWork DensityOperation::work(std::shared_ptr<const DensityBuildSnapshot> snapshot,
                                    std::shared_ptr<TestHooks> hooks)
{
    return [snapshot = std::move(snapshot), hooks = std::move(hooks)]
        (const OperationSnapshot &operation, const WorkerControl &control) -> WorkerOutcome {
        if (control.cancelled()) return WorkerOutcome::cancelled();
        if (!snapshot || operation.kind != OperationKind::DensityBuild || operation.density != snapshot)
            return WorkerOutcome::failed(OperationFailure::make(FailureReason::InvalidProduct,
                OperationPhase::Preparing, "Density worker snapshot/kind mismatch"));
        control.report({OperationPhase::Preparing, 0, std::nullopt});
        const auto sourcePath = QString::fromStdWString(snapshot->source.cachePath().wstring());
        if (!snapshot->source.location || !fullyQualified(sourcePath) || !fullyQualified(snapshot->outputScope)
            || snapshot->outputScope.isEmpty() || separator(snapshot->outputScope.back()))
            return failed({BuildErrorCode::InvalidInput, BuildPhase::Preflight,
                QStringLiteral("Density source and scope must be fully qualified Windows paths")});
        auto product = std::make_unique<WorkerDensityProduct>();
        product->diagnostics = std::make_shared<DensityOperationDiagnostics>();
        product->directory = std::make_unique<DensityAttemptDirectory>(snapshot->outputScope, product->diagnostics);
        // Keep guards even on failure until the runner's worker-side cleanup, so
        // secondary late PendingDataset/directory diagnostics remain observable.
        const auto retain = [&product](WorkerOutcome outcome) {
            outcome.density = std::move(product);
            return outcome;
        };
        try {
            if (!product->directory->create())
                return retain(failed({BuildErrorCode::ScratchIO, BuildPhase::Preflight,
                    QStringLiteral("Cannot create unique Density attempt directory")}));
            const auto path = snapshot->outputScope + QStringLiteral("/counts.pc2ddensity");
            Bridge bridge{control, snapshot->source.pointCount, product->diagnostics, hooks ? &hooks->progress : nullptr};
            density::BuildRequest request{.source = ImportedDataset{
                    .cacheId = snapshot->source.cacheId, .location = snapshot->source.location,
                    .pointCount = snapshot->source.pointCount, .minimum = snapshot->source.minimum,
                    .maximum = snapshot->source.maximum}, .cell = snapshot->cell,
                .tileSide = snapshot->tileSide, .groupSide = snapshot->groupSide,
                .eventCapacity = snapshot->eventCapacity, .outputPath = path,
                .scratchParent = product->directory->scratchPath(), .resources = snapshot->resources,
                .cancel = Bridge::cancel, .progress = Bridge::progress, .context = &bridge,
                .diagnostic = Bridge::diagnostic, .diagnosticContext = product->diagnostics};
            auto built = density::DensityBuilder::build(request);
            // An I/O/integrity/resource error is primary even when a later cancel
            // or failed progress bridge also requests cooperative stop.
            if (!built && built.error().code != BuildErrorCode::Cancelled)
                return retain(failed(built.error()));
            if (product->diagnostics->bridgeFailed)
                return retain(failed({BuildErrorCode::DensityFailure, product->diagnostics->bridgePhase,
                    QStringLiteral("Density progress bridge failed; computation stopped cooperatively")}));
            if (!built) {
                if (built.error().code == BuildErrorCode::Cancelled) return retain(WorkerOutcome::cancelled());
                return retain(failed(built.error()));
            }
            product->output = std::make_unique<density::BuildOutput>(std::move(built.value()));
            if (hooks && hooks->beforeReady) hooks->beforeReady();
            product->ready = std::make_shared<const DensityReadyProduct>(DensityReadyProduct{
                snapshot, product->output->dataset.metadata(), path, product->output->statistics});
            return WorkerOutcome::success(std::move(product));
        } catch (const std::bad_alloc &) {
            return retain(WorkerOutcome::failed(OperationFailure::make(FailureReason::OutOfMemory,
                OperationPhase::Finalizing)));
        } catch (const std::exception &error) {
            return retain(WorkerOutcome::failed(OperationFailure::make(FailureReason::DensityFailed,
                OperationPhase::Finalizing, error.what())));
        } catch (...) {
            return retain(WorkerOutcome::failed(OperationFailure::make(FailureReason::DensityFailed,
                OperationPhase::Finalizing, "Density adapter failure")));
        }
    };
}
OperationWork densityBuildWork(std::shared_ptr<const DensityBuildSnapshot> snapshot)
{ return DensityOperation::work(std::move(snapshot)); }

} // namespace cadcontour
