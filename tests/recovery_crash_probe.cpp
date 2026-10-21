#include "operation_test_support.h"
#include "recovery_test_access.h"

#include <QCoreApplication>
#include <QFile>

#define NOMINMAX
#include <Windows.h>

using namespace cadcontour;
using namespace cadcontour::test;
using namespace cadcontour::persistence;
namespace {
void hold(const QString &path)
{
    QFile marker(path);
    if (!marker.open(QIODevice::WriteOnly | QIODevice::NewOnly) || marker.write("ready") != 5 || !marker.flush())
        TerminateProcess(GetCurrentProcess(), 7);
    marker.close();
    Sleep(INFINITE); // parent kills this isolated process, bypassing all destructors
}
}
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc != 4) return 2;
    const auto args = app.arguments();
    ProjectController controller(args[1] + QStringLiteral("/imports"));
    const auto mode = args[2], marker = args[3];
    if (mode == u"restore") {
        auto found = discoverRecovery(RecoveryTestAccess::root(controller));
        if (!found || found.value().size() != 1) return 8;
        RecoveryRequest request{found.value()[0]};
        if (controller.restoreRecovery(request).outcome != CommandOutcome::Applied || !finish(controller)
            || controller.operation().status != OperationStatus::Succeeded) return 9;
        RecoveryTestAccess::timers(controller);
        hold(marker);
    }
    QFile source(args[1] + QStringLiteral("/source.xyz"));
    if (!source.open(QIODevice::WriteOnly) || source.write("1 2 0\n3 4 0\n") != 12) return 3;
    source.close();
    if (controller.importAscXyz({source.fileName(), {8ULL << 20, 64ULL << 20, 8}}).outcome != CommandOutcome::Applied
        || !finish(controller) || controller.operation().status != OperationStatus::Succeeded) return 4;
    RecoveryTestAccess::timers(controller);
    if (mode != u"first" && (controller.autosave().outcome != CommandOutcome::Applied || !finish(controller)
        || controller.operation().status != OperationStatus::Succeeded)) return 5;
    if (mode == u"active") hold(marker);
    controller.setCell(0.125);
    auto hooks = std::make_shared<RecoveryTransactionTestAccess::Hooks>();
    hooks->phase = [mode, marker](RecoveryPhase phase) {
        if (((mode == u"before" || mode == u"first") && phase == RecoveryPhase::BeforeCommit)
            || (mode == u"published" && phase == RecoveryPhase::Published)) hold(marker);
    };
    if (mode == u"write") hooks->write = [marker](QIODevice &file, const char *bytes, qint64 size) -> qint64 {
        file.write(bytes, size / 2);
        if (auto *device = dynamic_cast<QFileDevice *>(&file)) device->flush();
        hold(marker); return -1;
    };
    RecoveryTestAccess::configure(controller, hooks);
    controller.autosave();
    return finish(controller) ? 6 : 10;
}
