import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import CADContour2D

ApplicationWindow {
    id: window
    required property ProjectViewModel project

    width: 680
    height: 460
    minimumWidth: 400
    minimumHeight: 380
    visible: true
    title: "CADContour2D"

    Connections {
        target: window.project
        function onCellInputSyncRequested() {
            cellInput.text = window.project.cellText
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 24
        spacing: 12

        Label {
            objectName: "projectStatus"
            text: window.project.hasProject ? "Новый проект" : "Нет открытого проекта"
            font.pixelSize: 24
            Layout.fillWidth: true
        }

        Label {
            objectName: "identityLabel"
            text: window.project.hasProject
                  ? "Экземпляр: " + window.project.runtimeIdentity
                    + " · Ревизия входов: " + window.project.inputRevision : ""
            wrapMode: Text.Wrap
            Layout.fillWidth: true
        }

        Label {
            objectName: "cellValueLabel"
            text: window.project.hasProject ? "Размер ячейки: " + window.project.cellText + " мм" : ""
            wrapMode: Text.Wrap
            Layout.fillWidth: true
        }

        RowLayout {
            Layout.fillWidth: true
            TextField {
                id: cellInput
                objectName: "cellInput"
                Layout.fillWidth: true
                enabled: window.project.hasProject
                placeholderText: "cell, мм"
                Accessible.name: "Размер ячейки в миллиметрах"
                selectByMouse: true
                Component.onCompleted: cellInput.text = window.project.cellText
                onAccepted: window.project.applyCell(cellInput.text)
            }
            Button {
                objectName: "applyButton"
                text: "Применить"
                enabled: window.project.hasProject
                onClicked: window.project.applyCell(cellInput.text)
            }
        }

        Label {
            objectName: "densityStatus"
            text: window.project.densityMapStatusText
            Layout.fillWidth: true
            wrapMode: Text.Wrap
        }

        Label {
            objectName: "errorLabel"
            text: window.project.errorMessage
            visible: text.length > 0
            color: "#b42318"
            Layout.fillWidth: true
            wrapMode: Text.Wrap
        }

        Item { Layout.fillHeight: true }

        RowLayout {
            Layout.fillWidth: true
            Button {
                objectName: "undoButton"
                text: "Отменить"
                enabled: window.project.canUndo
                onClicked: window.project.undo()
            }
            Button {
                objectName: "redoButton"
                text: "Повторить"
                enabled: window.project.canRedo
                onClicked: window.project.redo()
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Button {
                objectName: "newProjectButton"
                text: "Новый проект"
                onClicked: window.project.newProject()
            }
            Item { Layout.fillWidth: true }
            Button {
                objectName: "closeButton"
                text: "Закрыть"
                onClicked: window.close()
            }
        }
    }
}
