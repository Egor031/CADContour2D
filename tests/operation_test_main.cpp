#include <QCoreApplication>
#include <gtest/gtest.h>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("CADContour2D-operation-tests"));
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
