#include "operation_test_support.h"
#include "save_test_access.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QFile>

#define NOMINMAX
#include <Windows.h>

using namespace cadcontour;
using namespace cadcontour::test;
using namespace cadcontour::persistence;

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc != 4 && argc != 5) return 2;
    const auto args = app.arguments();
    ProjectController c(QFileInfo(args[2]).absolutePath() + QStringLiteral("/import"));
    c.importAscXyz({args[1], {8ULL << 20, 64ULL << 20, 8}});
    if (!finish(c) || c.operation().status != OperationStatus::Succeeded) return 3;
    if (argc == 5) {
        c.saveProject({args[2]});
        if (!finish(c) || c.operation().status != OperationStatus::Succeeded) return 6;
        c.setCell(.5);
    }
    SaveTestAccess::configure(c, [phase = args[3], target = args[2], writes = 0](SavePhase p) mutable {
        if (phase == u"hold" && p == SavePhase::BeforeCommit) {
            QFile marker(target + QStringLiteral(".ready"));
            if (!marker.open(QIODevice::WriteOnly | QIODevice::NewOnly) || marker.write("ready") != 5 || !marker.flush())
                TerminateProcess(GetCurrentProcess(), 7);
            marker.close();
            Sleep(INFINITE);
        }
        if ((phase == u"copy" && p == SavePhase::CopyWrite && ++writes == 2)
            || (phase == u"dataset" && p == SavePhase::DatasetPublished)
            || (phase == u"commit" && p == SavePhase::ManifestCommit)
            || (phase == u"receipt" && p == SavePhase::Receipt)) TerminateProcess(GetCurrentProcess(), 73);
    });
    c.saveProject({args[2]});
    return finish(c) ? 4 : 5;
}
