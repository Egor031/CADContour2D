#include "operation_probe.h"
#include "operation_test_access.h"

namespace cadcontour {

OperationProbe::OperationProbe(ProjectController &controller) : controller_(controller)
{
    connect(&controller_, &ProjectController::operationChanged, this, &OperationProbe::changed);
    connect(&controller_, &ProjectController::stateChanged, this, &OperationProbe::changed);
    connect(&controller_, &ProjectController::shutdownChanged, this, &OperationProbe::changed);
}
bool OperationProbe::canStart() const noexcept
{
    return controller_.project() && !controller_.operation().active
        && controller_.shutdownState() == ShutdownState::Open;
}
void OperationProbe::startKnown() { start(false, false); }
void OperationProbe::startUnknown() { start(true, false); }
void OperationProbe::startFailure() { start(false, true); }
void OperationProbe::start(bool unknown, bool fail)
{
    OperationTestAccess::start(controller_, [unknown, fail](const auto &snapshot, const WorkerControl &control) {
        for (std::uint64_t i = 0; i <= 80; ++i) {
            if (control.cancelled())
                return WorkerOutcome::cancelled();
            const auto phase = i < 12 ? OperationPhase::Preparing : OperationPhase::Computing;
            control.report({phase, i, unknown || i < 12 ? std::nullopt : std::optional<std::uint64_t>{80}});
            // Deliberate demo pacing, not a synchronization assumption in regression tests.
            QThread::msleep(60);
            if (fail && i == 35)
                return WorkerOutcome::failed(OperationFailure::make(FailureReason::InvalidProduct,
                                             OperationPhase::Computing, "injected verification failure"));
        }
        auto product = std::make_unique<VerificationProduct>();
        product->sourceCell = snapshot.cell;
        product->value = 80;
        return WorkerOutcome::success(std::move(product));
    });
}

}
