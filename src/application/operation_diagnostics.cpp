#include "operation_diagnostics.h"

#include <QDir>
#include <QStandardPaths>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/spdlog.h>

namespace cadcontour {

void operationDiagnostic(const char *event, const OperationState &state,
                         std::int64_t elapsedMs) noexcept
{
    try {
        static const auto logger = []() -> std::shared_ptr<spdlog::logger> {
            try {
                const auto directory = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
                                     + QStringLiteral("/logs");
                if (!QDir().mkpath(directory))
                    return {};
                auto sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                    (directory + QStringLiteral("/operations.log")).toStdWString(), 1024 * 1024, 2);
                auto result = std::make_shared<spdlog::logger>("operations", std::move(sink));
                result->set_error_handler([](const std::string &) {});
                result->flush_on(spdlog::level::info);
                return result;
            } catch (...) { return {}; }
        }();
        if (!logger)
            return;
        logger->info("{} id={} project={} revision={} status={} worker={} decision={} ms={} "
                     "revoked={} cancel={} cleanupWarning={} reason={} phase={} systemCode={} observationError={} detail={}",
            event, state.id, state.stamp.projectIdentity, state.stamp.inputRevision,
            int(state.status), state.workerStatus ? int(*state.workerStatus) : -1,
            int(state.decision), elapsedMs, state.publicationRevoked, state.cancellationRequested,
            state.cleanupWarning, state.failure ? int(state.failure->reason) : -1,
            int(state.progress.phase), state.failure && state.failure->systemCode ? *state.failure->systemCode : 0,
            state.completionObservationError.value_or(0), state.failure ? state.failure->detail.data() : "");
    } catch (...) { /* Diagnostics must never change execution/publication safety. */ }
}

} // namespace cadcontour
