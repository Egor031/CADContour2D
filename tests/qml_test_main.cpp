#include <QGuiApplication>
#include <gtest/gtest.h>

#ifdef _MSC_VER
#include <crtdbg.h>
#endif

int main(int argc, char **argv)
{
#ifdef _MSC_VER
    // Unattended CTest must report assertions to stderr instead of opening a modal CRT window.
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    if (qgetenv("QT_QPA_PLATFORM") == "offscreen") {
        qputenv("QT_QUICK_BACKEND", "software");
        // NativeStyle requires the Windows platform; use portable controls offscreen.
        qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    }
    QGuiApplication app(argc, argv);
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
