import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import CADContour2D

ApplicationWindow {
    id: window
    required property ProjectViewModel project
    property Component developmentControls: null

    width: 680
    height: 600
    minimumWidth: 400
    minimumHeight: 380
    visible: true
    title: "CADContour2D"

    onClosing: function(close) {
        close.accepted = window.project.requestClose()
    }

    Connections {
        target: window.project
        function onCellInputSyncRequested() {
            cellInput.text = window.project.cellText
        }
        function onShutdownChanged() {
            if (window.project.shutdownReady)
                Qt.callLater(function() { window.close() })
        }
    }

    ScrollView {
        anchors.fill: parent
        anchors.margins: 16
        contentWidth: availableWidth
        clip: true

        ColumnLayout {
            width: parent.width
            spacing: 8

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
                    enabled: window.project.hasProject && window.project.actionsEnabled
                    placeholderText: "cell, мм"
                    Accessible.name: "Размер ячейки в миллиметрах"
                    selectByMouse: true
                    Component.onCompleted: cellInput.text = window.project.cellText
                    onAccepted: window.project.applyCell(cellInput.text)
                }
                Button {
                    objectName: "applyButton"
                    text: "Применить"
                    enabled: window.project.hasProject && window.project.actionsEnabled
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

            ColumnLayout {
                visible: window.project.operationVisible
                Layout.fillWidth: true
                spacing: 4
                Label {
                    objectName: "operationLabel"
                    text: window.project.operationText
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                }
                Label {
                    objectName: "operationPhase"
                    text: window.project.phaseText + " · " + window.project.progressText
                    visible: window.project.operationActive
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                }
                ProgressBar {
                    objectName: "operationProgress"
                    visible: window.project.operationActive
                    indeterminate: !window.project.progressKnown
                    value: window.project.progressValue
                    Layout.fillWidth: true
                }
                Button {
                    objectName: "stopOperationButton"
                    text: "Остановить операцию"
                    visible: window.project.operationActive
                    enabled: window.project.canCancelOperation
                    onClicked: window.project.cancelOperation()
                }
            }

            Loader {
                sourceComponent: window.developmentControls
                visible: sourceComponent !== null
                Layout.fillWidth: true
            }

            RowLayout {
                Layout.fillWidth: true
                Button {
                    objectName: "undoButton"
                    text: "Отменить"
                    enabled: window.project.canUndo && window.project.actionsEnabled
                    onClicked: window.project.undo()
                }
                Button {
                    objectName: "redoButton"
                    text: "Повторить"
                    enabled: window.project.canRedo && window.project.actionsEnabled
                    onClicked: window.project.redo()
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Button {
                    objectName: "newProjectButton"
                    text: "Новый проект"
                    enabled: window.project.actionsEnabled
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
}
