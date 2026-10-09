#include "ui/project_view_model.h"
#include "project_test_access.h"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>

using namespace cadcontour;

TEST(ProjectViewModel, ReflectsControllerWithoutIndependentParameters)
{
    ProjectController controller;
    ProjectViewModel viewModel(controller);
    EXPECT_FALSE(viewModel.hasProject());
    EXPECT_TRUE(viewModel.cellText().isEmpty());
    viewModel.applyCell(QStringLiteral("0,5"));
    EXPECT_FALSE(viewModel.errorMessage().isEmpty());
    controller.newProject();
    EXPECT_TRUE(viewModel.hasProject());
    EXPECT_EQ(viewModel.cellText(), QStringLiteral("1"));
    EXPECT_TRUE(viewModel.errorMessage().isEmpty());
    const auto identity = viewModel.runtimeIdentity();
    controller.setCell(2.5);
    EXPECT_DOUBLE_EQ(viewModel.cell(), 2.5);
    EXPECT_EQ(viewModel.inputRevision(), QStringLiteral("1"));
    EXPECT_TRUE(viewModel.canUndo());
    viewModel.undo();
    EXPECT_EQ(viewModel.cellText(), QStringLiteral("1"));
    EXPECT_TRUE(viewModel.canRedo());
    viewModel.redo();
    EXPECT_DOUBLE_EQ(controller.project()->cell(), 2.5);
    viewModel.newProject();
    EXPECT_NE(viewModel.runtimeIdentity(), identity);
    EXPECT_EQ(viewModel.cellText(), QStringLiteral("1"));
    EXPECT_FALSE(viewModel.canUndo());
    EXPECT_FALSE(viewModel.canRedo());
    EXPECT_EQ(viewModel.densityMapStatusText(), QStringLiteral("Карта плотности отсутствует"));
    controller.closeProject();
    EXPECT_FALSE(viewModel.hasProject());
    EXPECT_TRUE(viewModel.cellText().isEmpty());
    viewModel.undo();
    EXPECT_FALSE(viewModel.errorMessage().isEmpty());
    viewModel.redo();
    EXPECT_FALSE(viewModel.errorMessage().isEmpty());
}

namespace {
class ValidCellText : public testing::TestWithParam<const char *> {};
TEST_P(ValidCellText, SupportsDecimalSeparatorsTrimAndExponent)
{
    ProjectController controller;
    controller.newProject();
    ProjectViewModel viewModel(controller);
    viewModel.applyCell(QString::fromLatin1(GetParam()));
    EXPECT_TRUE(viewModel.errorMessage().isEmpty());
    EXPECT_DOUBLE_EQ(controller.project()->cell(), 0.5);
    EXPECT_EQ(viewModel.cellText(), QStringLiteral("0.5"));
    EXPECT_EQ(controller.project()->inputRevision(), 1);
}
INSTANTIATE_TEST_SUITE_P(Numbers, ValidCellText,
    testing::Values("0,5", "0.5", "  0,5  ", "+.5", ",5", "5e-1", "0,05E+1", "\t0.5\n"));

class InvalidCellText : public testing::TestWithParam<const char *> {};
TEST_P(InvalidCellText, PreservesRedoAndDoesNotSynchronizeDraft)
{
    ProjectController controller;
    controller.newProject();
    controller.setCell(0.5);
    controller.undo();
    ProjectViewModel viewModel(controller);
    int notifications = 0;
    int synchronizations = 0;
    QObject::connect(&viewModel, &ProjectViewModel::stateChanged, [&] { ++notifications; });
    QObject::connect(&viewModel, &ProjectViewModel::cellInputSyncRequested, [&] { ++synchronizations; });
    viewModel.applyCell(QString::fromLatin1(GetParam()));
    EXPECT_FALSE(viewModel.errorMessage().isEmpty());
    EXPECT_DOUBLE_EQ(controller.project()->cell(), 1.0);
    EXPECT_EQ(controller.project()->inputRevision(), 2);
    EXPECT_TRUE(viewModel.canRedo());
    EXPECT_EQ(notifications, 0);
    EXPECT_EQ(synchronizations, 0);
    viewModel.redo();
    EXPECT_DOUBLE_EQ(controller.project()->cell(), 0.5);
    EXPECT_TRUE(viewModel.errorMessage().isEmpty());
}
INSTANTIATE_TEST_SUITE_P(MalformedOrOutOfRange, InvalidCellText, testing::Values(
    "", " ", "abc", "0", "-0", "-0.5", "nan", "NaN", "inf", "Infinity", "1e309",
    "1e-999", "1,2.3", "1,2,3", "1.2.3", "1 000", "1_000", "0x1p-1", "1e", "1e+", "--1"));
} // namespace

TEST(ProjectViewModel, FormattingRoundTripsDoubleExtremes)
{
    ProjectController controller;
    controller.newProject();
    ProjectViewModel viewModel(controller);
    for (double value : {std::numeric_limits<double>::denorm_min(),
                         std::numeric_limits<double>::min(), std::numeric_limits<double>::max(),
                         std::nextafter(1.0, 2.0), 0.1, 1e-100, 1e100}) {
        controller.setCell(value);
        const QString formatted = viewModel.cellText();
        controller.setCell(1.0);
        viewModel.applyCell(formatted);
        EXPECT_TRUE(viewModel.errorMessage().isEmpty()) << formatted.toStdString();
        EXPECT_EQ(controller.project()->cell(), value) << formatted.toStdString();
    }
}

TEST(ProjectViewModel, NoOpCanonicalizesInputWithoutChangingHistory)
{
    ProjectController controller;
    controller.newProject();
    controller.setCell(0.5);
    controller.undo();
    ProjectViewModel viewModel(controller);
    int synchronizations = 0;
    QObject::connect(&viewModel, &ProjectViewModel::cellInputSyncRequested, [&] { ++synchronizations; });
    viewModel.applyCell(QStringLiteral("invalid"));
    viewModel.applyCell(QStringLiteral(" 1,0 "));
    EXPECT_TRUE(viewModel.errorMessage().isEmpty());
    EXPECT_EQ(synchronizations, 1);
    EXPECT_EQ(controller.project()->inputRevision(), 2);
    EXPECT_TRUE(viewModel.canRedo());
    EXPECT_EQ(ProjectControllerTestAccess::count(controller), 1);
}

TEST(ProjectViewModel, NotificationsAreConsistentAndRejectNestedCommands)
{
    ProjectController controller;
    controller.newProject();
    ProjectViewModel viewModel(controller);
    int notifications = 0;
    QObject::connect(&viewModel, &ProjectViewModel::stateChanged, [&] {
        ++notifications;
        EXPECT_DOUBLE_EQ(viewModel.cell(), controller.project()->cell());
        EXPECT_EQ(viewModel.canUndo(), controller.canUndo());
        EXPECT_EQ(viewModel.canRedo(), controller.canRedo());
        viewModel.applyCell(QStringLiteral("8"));
        EXPECT_DOUBLE_EQ(viewModel.cell(), controller.project()->cell());
    });
    viewModel.applyCell(QStringLiteral("0,5"));
    EXPECT_EQ(notifications, 1);
    EXPECT_DOUBLE_EQ(viewModel.cell(), 0.5);
    EXPECT_EQ(controller.project()->inputRevision(), 1);
}

TEST(ProjectViewModel, CounterFailuresProduceReadableErrorsWithoutInputReset)
{
    ProjectController controller;
    controller.newProject();
    controller.setCell(0.5);
    ProjectControllerTestAccess::setRevision(controller, std::numeric_limits<std::uint64_t>::max());
    ProjectViewModel viewModel(controller);
    int resets = 0;
    QObject::connect(&viewModel, &ProjectViewModel::cellInputSyncRequested, [&] { ++resets; });
    viewModel.applyCell(QStringLiteral("2"));
    EXPECT_FALSE(viewModel.errorMessage().isEmpty());
    viewModel.undo();
    EXPECT_FALSE(viewModel.errorMessage().isEmpty());
    EXPECT_EQ(resets, 0);
    EXPECT_DOUBLE_EQ(controller.project()->cell(), 0.5);
}
