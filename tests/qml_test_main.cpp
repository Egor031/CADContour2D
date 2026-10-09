#include <QGuiApplication>
#include <gtest/gtest.h>

int main(int argc, char **argv)
{
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
