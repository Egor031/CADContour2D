#include "ui/project_view_model.h"

#include <QCoreApplication>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQuickItem>
#include <QQuickWindow>
#include <QtQml/qqml.h>
#include <gtest/gtest.h>

#include <memory>
#include <vector>

using namespace cadcontour;

namespace {
class ProductionQml : public testing::Test {
protected:
    ProjectController controller;
    ProjectViewModel viewModel{controller};
    QQmlApplicationEngine engine;
    QQuickWindow *window = nullptr;
    std::vector<QString> warnings;

    void SetUp() override
    {
        controller.newProject();
        QQmlEngine::setObjectOwnership(&viewModel, QQmlEngine::CppOwnership);
        QObject::connect(&engine, &QQmlEngine::warnings, [&] (const QList<QQmlError> &errors) {
            for (const auto &error : errors)
                warnings.push_back(error.toString());
        });
        engine.setInitialProperties({{QStringLiteral("project"), QVariant::fromValue(&viewModel)}});
        engine.loadFromModule("CADContour2D", "Main");
        ASSERT_EQ(engine.rootObjects().size(), 1);
        window = qobject_cast<QQuickWindow *>(engine.rootObjects().front());
        ASSERT_NE(window, nullptr);
        QCoreApplication::processEvents();
        ASSERT_TRUE(warnings.empty());
    }

    void TearDown() override
    {
        for (const auto &warning : warnings)
            ADD_FAILURE() << warning.toStdString();
        // QObject connections capture this fixture; engine dies before its members.
        QObject::disconnect(&engine, nullptr, nullptr, nullptr);
    }

    QObject *control(const char *name)
    {
        auto *object = window->findChild<QObject *>(QString::fromLatin1(name));
        EXPECT_NE(object, nullptr) << name;
        return object;
    }

    QString text(const char *name) { return control(name)->property("text").toString(); }
    bool enabled(const char *name) { return control(name)->property("enabled").toBool(); }
    void draft(const QString &value) { ASSERT_TRUE(control("cellInput")->setProperty("text", value)); }
    void click(const char *name)
    {
        // Invoke the real control's signal, exercising the production onClicked handler.
        ASSERT_TRUE(QMetaObject::invokeMethod(control(name), "clicked", Qt::DirectConnection));
        QCoreApplication::processEvents();
    }
};
} // namespace

TEST_F(ProductionQml, CompleteUserScenarioAndRejectedDraft)
{
    EXPECT_TRUE(window->isVisible());
    EXPECT_EQ(text("cellInput"), QStringLiteral("1"));
    EXPECT_EQ(text("cellValueLabel"), QStringLiteral("Размер ячейки: 1 мм"));
    EXPECT_EQ(text("densityStatus"), QStringLiteral("Карта плотности отсутствует"));
    EXPECT_FALSE(enabled("undoButton"));
    EXPECT_FALSE(enabled("redoButton"));
    const auto identity = controller.project()->runtimeIdentity();
    draft(QStringLiteral("0,5"));
    click("applyButton");
    EXPECT_DOUBLE_EQ(controller.project()->cell(), 0.5);
    EXPECT_EQ(text("cellInput"), QStringLiteral("0.5"));
    EXPECT_EQ(text("cellValueLabel"), QStringLiteral("Размер ячейки: 0.5 мм"));
    EXPECT_TRUE(enabled("undoButton"));
    EXPECT_FALSE(enabled("redoButton"));
    click("undoButton");
    EXPECT_EQ(text("cellInput"), QStringLiteral("1"));
    EXPECT_FALSE(enabled("undoButton"));
    EXPECT_TRUE(enabled("redoButton"));
    draft(QStringLiteral("1,2.3"));
    click("applyButton");
    EXPECT_EQ(text("cellInput"), QStringLiteral("1,2.3"));
    EXPECT_FALSE(text("errorLabel").isEmpty());
    EXPECT_TRUE(control("errorLabel")->property("visible").toBool());
    EXPECT_DOUBLE_EQ(controller.project()->cell(), 1);
    EXPECT_TRUE(enabled("redoButton"));
    draft(QStringLiteral(" 1,0 "));
    click("applyButton");
    EXPECT_EQ(text("cellInput"), QStringLiteral("1"));
    EXPECT_TRUE(text("errorLabel").isEmpty());
    EXPECT_TRUE(enabled("redoButton"));
    click("redoButton");
    EXPECT_EQ(text("cellInput"), QStringLiteral("0.5"));
    click("newProjectButton");
    EXPECT_EQ(text("cellInput"), QStringLiteral("1"));
    EXPECT_GT(controller.project()->runtimeIdentity(), identity);
    EXPECT_EQ(controller.project()->inputRevision(), 0);
    EXPECT_FALSE(enabled("undoButton"));
    EXPECT_FALSE(enabled("redoButton"));
    click("closeButton");
    EXPECT_FALSE(window->isVisible());
}

TEST_F(ProductionQml, EnterHandlerAndControllerChangesSynchronizeInput)
{
    draft(QStringLiteral("2,5"));
    ASSERT_TRUE(QMetaObject::invokeMethod(control("cellInput"), "accepted", Qt::DirectConnection));
    EXPECT_DOUBLE_EQ(controller.project()->cell(), 2.5);
    EXPECT_EQ(text("cellInput"), QStringLiteral("2.5"));
    controller.undo();
    EXPECT_EQ(text("cellInput"), QStringLiteral("1"));
    controller.closeProject();
    EXPECT_EQ(text("projectStatus"), QStringLiteral("Нет открытого проекта"));
    EXPECT_TRUE(text("cellInput").isEmpty());
    EXPECT_FALSE(enabled("cellInput"));
    EXPECT_FALSE(enabled("applyButton"));
    click("newProjectButton");
    EXPECT_TRUE(enabled("applyButton"));
    EXPECT_EQ(text("cellInput"), QStringLiteral("1"));
}

TEST_F(ProductionQml, ResizedLayoutKeepsControlsInsideWindow)
{
    draft(QStringLiteral("1,2.3"));
    click("applyButton");
    for (const QSize size : {QSize{400, 380}, QSize{1000, 700}}) {
        window->resize(size);
        QCoreApplication::processEvents();
        for (const char *name : {"cellInput", "applyButton", "errorLabel", "undoButton",
                                "redoButton", "newProjectButton", "closeButton"}) {
            auto *item = qobject_cast<QQuickItem *>(control(name));
            ASSERT_NE(item, nullptr);
            const auto bounds = item->mapRectToScene(item->boundingRect());
            EXPECT_GE(bounds.left(), 0) << name;
            EXPECT_GE(bounds.top(), 0) << name;
            EXPECT_LE(bounds.right(), window->width()) << name;
            EXPECT_LE(bounds.bottom(), window->height()) << name;
            EXPECT_GT(item->width(), 0) << name;
            EXPECT_GT(item->height(), 0) << name;
        }
    }
}

TEST(QmlRegistration, ViewModelCannotBeCreatedByQml)
{
    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData("import CADContour2D\nProjectViewModel {}", QUrl{});
    EXPECT_TRUE(component.isError());
    EXPECT_TRUE(component.errorString().contains(QStringLiteral("supplied by the application")));
}

TEST(QmlRegistration, MainRequiresTypedInjection)
{
    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.loadFromModule("CADContour2D", "Main");
    ASSERT_TRUE(component.isReady()) << component.errorString().toStdString();
    std::unique_ptr<QObject> missing(component.createWithInitialProperties({}));
    EXPECT_EQ(missing, nullptr);
    EXPECT_TRUE(component.errorString().contains(QStringLiteral("Required property project")));
}
