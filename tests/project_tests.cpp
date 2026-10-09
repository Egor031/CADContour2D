#include "core/project_state.h"
#include "application/project_controller.h"
#include "project_test_access.h"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <vector>

using namespace cadcontour;

namespace {
constexpr auto maxRevision = std::numeric_limits<std::uint64_t>::max();
constexpr double infinity = std::numeric_limits<double>::infinity();
constexpr double nan = std::numeric_limits<double>::quiet_NaN();

class InvalidCell : public testing::TestWithParam<double> {};
TEST_P(InvalidCell, PreservesDomainAndHistory)
{
    ProjectState state(1);
    const auto result = state.changeCell(GetParam());
    ASSERT_EQ(result.outcome, CommandOutcome::Rejected);
    ASSERT_TRUE(result.failure);
    EXPECT_EQ(result.failure->category, CommandError::InvalidValue);
    EXPECT_EQ(result.failure->detail, ErrorDetail::CellMustBeFiniteAndPositive);
    EXPECT_TRUE(result.failure->attemptedValue);
    EXPECT_DOUBLE_EQ(state.cell(), 1.0);
    EXPECT_EQ(state.inputRevision(), 0);

    ProjectController controller;
    controller.newProject();
    controller.setCell(0.5);
    controller.undo();
    EXPECT_EQ(controller.setCell(GetParam()).outcome, CommandOutcome::Rejected);
    EXPECT_DOUBLE_EQ(controller.project()->cell(), 1.0);
    EXPECT_EQ(controller.project()->inputRevision(), 2);
    EXPECT_EQ(ProjectControllerTestAccess::count(controller), 1);
    EXPECT_TRUE(controller.canRedo());
    controller.redo();
    EXPECT_DOUBLE_EQ(controller.project()->cell(), 0.5);
}
INSTANTIATE_TEST_SUITE_P(NonPositiveOrNonFinite, InvalidCell,
    testing::Values(0.0, -0.0, -1.0, -1e-300, nan, infinity, -infinity));

class ValidCell : public testing::TestWithParam<double> {};
TEST_P(ValidCell, AcceptsEveryRepresentablePositiveScale)
{
    ProjectState state(1);
    const auto result = state.changeCell(GetParam());
    EXPECT_EQ(result.outcome, GetParam() == 1.0 ? CommandOutcome::NoChange : CommandOutcome::Applied);
    EXPECT_FALSE(result.failure);
    EXPECT_DOUBLE_EQ(state.cell(), GetParam());
}
INSTANTIATE_TEST_SUITE_P(Positive, ValidCell, testing::Values(
    1.0, 0.5, std::numeric_limits<double>::denorm_min(),
    std::numeric_limits<double>::min(), std::numeric_limits<double>::max(), 1e-200, 1e200));
} // namespace

TEST(ProjectState, InitialStateAndIdentityInvariant)
{
    ProjectState state(17);
    EXPECT_DOUBLE_EQ(state.cell(), 1.0);
    EXPECT_EQ(state.runtimeIdentity(), 17);
    EXPECT_EQ(state.inputRevision(), 0);
    EXPECT_TRUE(state.accepts({17, 0}));
    EXPECT_FALSE(state.accepts({18, 0}));
    EXPECT_THROW(ProjectState(0), std::invalid_argument);
}

TEST(ProjectState, RevisionPreflightAndExhaustion)
{
    ProjectState state(1);
    ProjectStateTestAccess::setRevision(state, maxRevision - 1);
    EXPECT_EQ(state.checkCellChange(0.5).outcome, CommandOutcome::Applied);
    EXPECT_EQ(state.inputRevision(), maxRevision - 1);
    EXPECT_EQ(state.changeCell(0.5).outcome, CommandOutcome::Applied);
    EXPECT_EQ(state.inputRevision(), maxRevision);
    EXPECT_EQ(state.changeCell(0.5).outcome, CommandOutcome::NoChange);
    auto result = state.changeCell(1.0);
    ASSERT_EQ(result.outcome, CommandOutcome::Rejected);
    EXPECT_EQ(result.failure->detail, ErrorDetail::RevisionExhausted);
    EXPECT_EQ(state.inputRevision(), maxRevision);
    EXPECT_DOUBLE_EQ(state.cell(), 0.5);
}

TEST(ResultStatus, PresenceAndCompatibilityAreIndependentOfWorkerRevisions)
{
    EXPECT_EQ(resultStatus(false, false), ResultStatus::Missing);
    EXPECT_EQ(resultStatus(false, true), ResultStatus::Missing);
    EXPECT_EQ(resultStatus(true, false), ResultStatus::Outdated);
    EXPECT_EQ(resultStatus(true, true), ResultStatus::Current);
    ProjectState state(1);
    const auto oldStamp = state.operationStamp();
    state.changeCell(0.5);
    state.changeCell(1.0);
    EXPECT_FALSE(state.accepts(oldStamp));
    // A saved result with matching source/build inputs can still be reusable.
    EXPECT_EQ(resultStatus(true, true), ResultStatus::Current);
}

TEST(ProjectController, NoProjectAndEmptyHistory)
{
    ProjectController controller;
    EXPECT_EQ(controller.project(), nullptr);
    EXPECT_FALSE(controller.canUndo());
    EXPECT_FALSE(controller.canRedo());
    for (auto result : {controller.setCell(0.5), controller.undo(), controller.redo()}) {
        EXPECT_EQ(result.outcome, CommandOutcome::Rejected);
        ASSERT_TRUE(result.failure);
        EXPECT_EQ(result.failure->category, CommandError::NoProject);
    }
    EXPECT_EQ(controller.closeProject().outcome, CommandOutcome::NoChange);
    EXPECT_EQ(controller.newProject().outcome, CommandOutcome::Applied);
    EXPECT_EQ(controller.undo().outcome, CommandOutcome::NoChange);
    EXPECT_EQ(controller.redo().outcome, CommandOutcome::NoChange);
}

TEST(ProjectController, PushUndoRedoAndBranching)
{
    ProjectController controller;
    controller.newProject();
    const auto identity = controller.project()->runtimeIdentity();
    const auto stamp = controller.project()->operationStamp();
    EXPECT_EQ(controller.setCell(0.5).outcome, CommandOutcome::Applied);
    EXPECT_EQ(controller.project()->inputRevision(), 1); // push performs redo exactly once
    EXPECT_EQ(controller.setCell(0.25).outcome, CommandOutcome::Applied);
    EXPECT_EQ(ProjectControllerTestAccess::count(controller), 2);
    controller.undo();
    EXPECT_DOUBLE_EQ(controller.project()->cell(), 0.5);
    controller.undo();
    EXPECT_DOUBLE_EQ(controller.project()->cell(), 1.0);
    EXPECT_EQ(controller.project()->inputRevision(), 4);
    EXPECT_FALSE(controller.project()->accepts(stamp));
    controller.redo();
    EXPECT_DOUBLE_EQ(controller.project()->cell(), 0.5);
    EXPECT_EQ(controller.project()->inputRevision(), 5);
    EXPECT_EQ(controller.setCell(0.5).outcome, CommandOutcome::NoChange);
    EXPECT_EQ(controller.project()->inputRevision(), 5);
    EXPECT_TRUE(controller.canRedo());
    EXPECT_EQ(ProjectControllerTestAccess::count(controller), 2);
    controller.setCell(2.0);
    EXPECT_FALSE(controller.canRedo());
    EXPECT_EQ(ProjectControllerTestAccess::count(controller), 2);
    controller.undo();
    EXPECT_DOUBLE_EQ(controller.project()->cell(), 0.5);
    controller.redo();
    EXPECT_DOUBLE_EQ(controller.project()->cell(), 2.0);
    EXPECT_EQ(controller.project()->inputRevision(), 8);
    EXPECT_EQ(controller.project()->runtimeIdentity(), identity);
}

TEST(ProjectController, ReplacementAndCloseClearHistoryBeforeStateDestruction)
{
    ProjectController controller;
    controller.newProject();
    const auto first = controller.project()->operationStamp();
    controller.setCell(0.5);
    controller.undo();
    controller.newProject();
    EXPECT_GT(controller.project()->runtimeIdentity(), first.projectIdentity);
    EXPECT_EQ(controller.project()->inputRevision(), 0);
    EXPECT_DOUBLE_EQ(controller.project()->cell(), 1.0);
    EXPECT_FALSE(controller.project()->accepts(first));
    EXPECT_EQ(ProjectControllerTestAccess::count(controller), 0);
    EXPECT_FALSE(controller.canUndo());
    EXPECT_FALSE(controller.canRedo());
    const auto second = controller.project()->runtimeIdentity();
    controller.setCell(3.0);
    EXPECT_EQ(controller.closeProject().outcome, CommandOutcome::Applied);
    EXPECT_EQ(controller.project(), nullptr);
    EXPECT_EQ(ProjectControllerTestAccess::count(controller), 0);
    controller.newProject();
    EXPECT_GT(controller.project()->runtimeIdentity(), second);
}

TEST(ProjectController, ExhaustedRevisionPreservesBothBranches)
{
    ProjectController controller;
    controller.newProject();
    controller.setCell(0.5);
    controller.setCell(0.25);
    controller.undo();
    ProjectControllerTestAccess::setRevision(controller, maxRevision);
    int notifications = 0;
    QObject::connect(&controller, &ProjectController::stateChanged, [&] { ++notifications; });
    for (auto result : {controller.setCell(2.0), controller.undo(), controller.redo()}) {
        EXPECT_EQ(result.outcome, CommandOutcome::Rejected);
        ASSERT_TRUE(result.failure);
        EXPECT_EQ(result.failure->detail, ErrorDetail::RevisionExhausted);
    }
    EXPECT_EQ(controller.setCell(0.5).outcome, CommandOutcome::NoChange);
    EXPECT_DOUBLE_EQ(controller.project()->cell(), 0.5);
    EXPECT_EQ(controller.project()->inputRevision(), maxRevision);
    EXPECT_EQ(ProjectControllerTestAccess::index(controller), 1);
    EXPECT_EQ(ProjectControllerTestAccess::count(controller), 2);
    EXPECT_TRUE(controller.canUndo());
    EXPECT_TRUE(controller.canRedo());
    EXPECT_EQ(notifications, 0);
}

TEST(ProjectController, LastRevisionIsUsableExactlyOnce)
{
    ProjectController controller;
    controller.newProject();
    ProjectControllerTestAccess::setRevision(controller, maxRevision - 1);
    EXPECT_EQ(controller.setCell(2.0).outcome, CommandOutcome::Applied);
    EXPECT_EQ(controller.project()->inputRevision(), maxRevision);
    EXPECT_EQ(controller.undo().outcome, CommandOutcome::Rejected);
    EXPECT_DOUBLE_EQ(controller.project()->cell(), 2.0);
    EXPECT_EQ(ProjectControllerTestAccess::index(controller), 1);
}

TEST(ProjectController, IdentityExhaustionDoesNotReplaceProjectOrHistory)
{
    ProjectController controller;
    ProjectControllerTestAccess::setLastIdentity(controller, maxRevision - 1);
    ASSERT_EQ(controller.newProject().outcome, CommandOutcome::Applied);
    EXPECT_EQ(controller.project()->runtimeIdentity(), maxRevision);
    controller.setCell(0.5);
    const auto stamp = controller.project()->operationStamp();
    const auto result = controller.newProject();
    ASSERT_EQ(result.outcome, CommandOutcome::Rejected);
    EXPECT_EQ(result.failure->detail, ErrorDetail::IdentityExhausted);
    EXPECT_TRUE(controller.project()->accepts(stamp));
    EXPECT_DOUBLE_EQ(controller.project()->cell(), 0.5);
    EXPECT_TRUE(controller.canUndo());
    controller.closeProject();
    EXPECT_EQ(controller.newProject().outcome, CommandOutcome::Rejected);
    EXPECT_EQ(controller.project(), nullptr);
}

TEST(ProjectController, NotificationsSeeSettledStateAndRejectReentry)
{
    ProjectController controller;
    using Snapshot = std::tuple<double, std::uint64_t, bool, bool>;
    std::vector<Snapshot> snapshots;
    QObject::connect(&controller, &ProjectController::stateChanged, [&] {
        const auto *state = controller.project();
        snapshots.emplace_back(state ? state->cell() : 0.0, state ? state->inputRevision() : 0,
                               controller.canUndo(), controller.canRedo());
        for (auto result : {controller.setCell(8.0), controller.undo(), controller.redo(),
                            controller.newProject(), controller.closeProject()}) {
            EXPECT_EQ(result.outcome, CommandOutcome::Rejected);
            ASSERT_TRUE(result.failure);
            EXPECT_EQ(result.failure->detail, ErrorDetail::ReentrantCommand);
        }
    });
    controller.newProject();
    controller.setCell(0.5);
    controller.setCell(0.5);
    controller.setCell(0.0);
    controller.undo();
    controller.redo();
    controller.newProject();
    controller.closeProject();
    const std::vector<Snapshot> expected{{1, 0, false, false}, {0.5, 1, true, false},
        {1, 2, false, true}, {0.5, 3, true, false}, {1, 0, false, false}, {0, 0, false, false}};
    EXPECT_EQ(snapshots, expected);
}
