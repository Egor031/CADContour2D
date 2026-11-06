#pragma once

#include "operation_runner.h"

namespace cadcontour {

OperationWork densityBuildWork(std::shared_ptr<const DensityBuildSnapshot> snapshot);

class DensityOperationTestAccess;
class DensityOperation {
public:
    static OperationWork work(std::shared_ptr<const DensityBuildSnapshot> snapshot);
private:
    friend class DensityOperationTestAccess;
    // Operation-local deterministic lifecycle/failure injection; production
    // request/snapshot never carries an arbitrary callback to the worker.
    struct TestHooks {
        std::function<void(density::BuildPhase)> progress;
        std::function<void()> beforeReady;
    };
    static OperationWork work(std::shared_ptr<const DensityBuildSnapshot> snapshot,
                              std::shared_ptr<TestHooks> hooks);
};

} // namespace cadcontour
