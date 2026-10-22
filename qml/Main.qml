pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs as NativeDialogs
import CADContour2D

ApplicationWindow {
    id: window
    required property ProjectViewModel project
    property Component developmentControls: null
    // Tests suppress only the OS window and deliver FileDialog.selectedFile/accepted.
    property bool fileDialogEnabled: true

    width: 960
    height: 720
    minimumWidth: 400
    minimumHeight: 380
    visible: true
    title: window.project.projectName + (window.project.dirty ? " *" : "") + " — CADContour2D"
    property bool diagnosticsVisible: false
    property NativeDialogs.FileDialog projectFileDialog: null
    property string projectDialogToken: ""

    function releaseProjectFileDialog(): void {
        const previous = window.projectFileDialog
        window.projectFileDialog = null
        window.projectDialogToken = ""
        if (previous) {
            previous.objectName = ""
            previous.close()
            previous.destroy()
        }
    }

    Action { id: newAction; text: "Новый проект / импорт ASC/XYZ…"; enabled: window.project.canImport; onTriggered: window.project.newProject() }
    Action { id: openAction; objectName: "openAction"; text: "Открыть проект…"; shortcut: StandardKey.Open; enabled: window.project.actionsEnabled && !window.project.workflowPending; onTriggered: window.project.open() }
    Action { id: saveAction; objectName: "saveAction"; text: "Сохранить"; shortcut: StandardKey.Save; enabled: window.project.canSave; onTriggered: window.project.save() }
    Action { id: saveAsAction; objectName: "saveAsAction"; text: "Сохранить как…"; shortcut: StandardKey.SaveAs; enabled: window.project.canSave; onTriggered: window.project.saveAs() }
    Action { id: closeProjectAction; objectName: "closeProjectAction"; text: "Закрыть проект"; enabled: window.project.hasProject && window.project.actionsEnabled; onTriggered: window.project.closeProject() }
    Action { id: exitAction; objectName: "exitAction"; text: "Выход"; onTriggered: window.close() }

    menuBar: MenuBar {
        Menu {
            title: "Файл"
            MenuItem { action: newAction }
            MenuItem { action: openAction }
            MenuSeparator {}
            MenuItem { action: saveAction }
            MenuItem { action: saveAsAction }
            MenuItem { action: closeProjectAction }
            MenuSeparator {}
            MenuItem { action: exitAction }
        }
        Menu {
            title: "Правка"
            MenuItem { text: "Отменить"; enabled: window.project.canUndo && window.project.cacheAvailable && window.project.actionsEnabled; onTriggered: window.project.undo() }
            MenuItem { text: "Повторить"; enabled: window.project.canRedo && window.project.cacheAvailable && window.project.actionsEnabled; onTriggered: window.project.redo() }
        }
        Menu {
            title: "Вид"
            MenuItem { text: "Диагностика"; checkable: true; checked: window.diagnosticsVisible; onTriggered: window.diagnosticsVisible = checked }
        }
    }
    header: ToolBar {
        Flow {
            width: parent.width
            ToolButton { action: openAction }
            ToolButton { action: saveAction }
            ToolButton { objectName: "diagnosticsButton"; text: "Диагностика"; onClicked: window.diagnosticsVisible = !window.diagnosticsVisible }
        }
    }
    function offerStartupImport(): void {
        if (window.project)
            window.project.offerStartupImport()
    }
    Component.onCompleted: Qt.callLater(window.offerStartupImport)

    onClosing: function(close) {
        close.accepted = window.project.requestClose()
    }

    Connections {
        target: window.project
        function onCellInputSyncRequested() {
            cellInput.text = window.project.cellDraft
        }
        function onShutdownChanged() {
            if (window.project.shutdownReady)
                Qt.callLater(function() { window.close() })
        }
        function onFileSelectionRequested() {
            const token = window.project.selectionToken
            if (!window.project.actionsEnabled || !window.project.selectingFile
                    || token.length === 0)
                return
            fileDialog.requestToken = token
            if (window.fileDialogEnabled)
                fileDialog.open()
        }
        function onImportUiChanged() {
            if (!window.project.selectingFile) {
                fileDialog.requestToken = ""
                fileDialog.close()
            }
            if (window.project.replacementPending) {
                if (!replacementDialog.visible) {
                    replacementDialog.requestToken = window.project.selectionToken
                    replacementDialog.open()
                }
            } else {
                replacementDialog.requestToken = ""
                replacementDialog.close()
            }
        }
        function onNonZeroZChanged() {
            const token = window.project.nonZeroZToken
            if (token.length > 0) {
                if (zDialog.requestToken !== token) {
                    zDialog.requestToken = token
                    zDialog.open()
                }
            } else {
                zDialog.requestToken = ""
                zDialog.close()
            }
        }
        function onWorkflowChanged() {
            const prompt = window.project.workflowPrompt
            const token = window.project.workflowToken
            if (prompt === "draft" || prompt === "dirty") {
                if (decisionDialog.requestToken !== token) {
                    decisionDialog.requestToken = token
                    decisionDialog.draft = prompt === "draft"
                    decisionDialog.open()
                }
            } else {
                decisionDialog.requestToken = ""
                decisionDialog.close()
            }
            if (prompt === "save" || prompt === "open") {
                if (!window.projectFileDialog || window.projectDialogToken !== token) {
                    window.releaseProjectFileDialog()
                    window.projectDialogToken = token
                    window.projectFileDialog = projectFileDialogComponent.createObject(window, {
                        "requestToken": token,
                        "fileMode": prompt === "save" ? NativeDialogs.FileDialog.SaveFile : NativeDialogs.FileDialog.OpenFile,
                        "title": prompt === "save" ? "Сохранить проект как" : "Открыть проект"
                    })
                    if (window.fileDialogEnabled)
                        window.projectFileDialog.open()
                }
            } else {
                window.releaseProjectFileDialog()
            }
        }
    }

    Component {
        id: projectFileDialogComponent
        NativeDialogs.FileDialog {
            objectName: "projectFileDialog"
            required property string requestToken
            nameFilters: ["Проект CADContour2D (*.pc2dproj)"]
            defaultSuffix: "pc2dproj"
            rejectLabel: "Отмена"
            onAccepted: window.project.selectProjectFile(requestToken, selectedFile)
            onRejected: window.project.answerWorkflow(requestToken, "cancel")
        }
    }

    Dialog {
        id: decisionDialog
        objectName: "decisionDialog"
        property string requestToken: ""
        property string closingToken: ""
        property bool draft: true
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(520, window.width - 32)
        height: Math.min(240, window.height - 32)
        modal: true
        title: draft ? "Неприменённые изменения" : "Несохранённые изменения"
        closePolicy: Popup.CloseOnEscape
        function answer(value: string): void {
            const token = requestToken
            requestToken = ""
            window.project.answerWorkflow(token, value)
        }
        onAboutToHide: closingToken = requestToken
        onClosed: {
            const token = closingToken
            closingToken = ""
            if (requestToken === token)
                requestToken = ""
            window.project.answerWorkflow(token, "cancel")
        }
        contentItem: Label {
            text: decisionDialog.draft ? "Размер ячейки изменён в поле ввода. Применить правку, отбросить её или отменить действие?"
                                      : "Сохранить изменения текущего проекта перед продолжением?"
            wrapMode: Text.Wrap
        }
        footer: DialogButtonBox {
            Button { objectName: "decisionAcceptButton"; text: decisionDialog.draft ? "Применить" : "Сохранить"; onClicked: decisionDialog.answer(decisionDialog.draft ? "apply" : "save") }
            Button { objectName: "decisionDiscardButton"; text: decisionDialog.draft ? "Отбросить" : "Не сохранять"; onClicked: decisionDialog.answer("discard") }
            Button { objectName: "decisionCancelButton"; text: "Отмена"; onClicked: decisionDialog.answer("cancel") }
        }
    }

    NativeDialogs.FileDialog {
        id: fileDialog
        objectName: "importFileDialog"
        property string requestToken: ""
        title: "Выберите облако точек ASC/XYZ"
        fileMode: NativeDialogs.FileDialog.OpenFile
        nameFilters: ["Облака точек (*.asc *.xyz)", "ASC (*.asc)", "XYZ (*.xyz)"]
        acceptLabel: "Импортировать"
        rejectLabel: "Отмена"
        onAccepted: window.project.selectImportFile(requestToken, selectedFile)
        onRejected: window.project.cancelFileSelection(requestToken)
    }

    Dialog {
        id: replacementDialog
        objectName: "replacementDialog"
        property string requestToken: ""
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(560, window.width - 32)
        height: Math.min(280, window.height - 32)
        modal: true
        title: "Заменить текущий проект?"
        closePolicy: Popup.CloseOnEscape
        onClosed: {
            const token = requestToken
            requestToken = ""
            window.project.confirmReplacement(token, false)
        }
        contentItem: Flickable {
            implicitHeight: 0
            contentWidth: width
            contentHeight: replacementText.implicitHeight
            boundsBehavior: Flickable.StopAtBounds
            clip: true
            ScrollBar.vertical: ScrollBar {}
            Label {
                id: replacementText
                width: replacementDialog.availableWidth
                textFormat: Text.PlainText
                text: "Выбран файл: " + window.project.replacementFileName
                    + "\n\nНовый проект заменит текущий только после успешного импорта. При отказе, ошибке или отмене текущий проект останется доступным."
                    + "\n\nПосле замены история Undo/Redo не переносится. Обычная отмена действия не восстановит предыдущий проект."
                wrapMode: Text.Wrap
            }
        }
        footer: DialogButtonBox {
            Button {
                objectName: "replaceConfirmButton"
                text: "Заменить проект"
                onClicked: {
                    const token = replacementDialog.requestToken
                    replacementDialog.requestToken = ""
                    window.project.confirmReplacement(token, true)
                    replacementDialog.close()
                }
            }
            Button {
                objectName: "replaceRejectButton"
                text: "Отказаться"
                onClicked: replacementDialog.close()
            }
        }
    }

    Dialog {
        id: zDialog
        objectName: "nonZeroZDialog"
        property string requestToken: ""
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(560, window.width - 32)
        height: Math.min(240, window.height - 32)
        modal: true
        title: "Ненулевая координата Z"
        closePolicy: Popup.CloseOnEscape
        onClosed: {
            const token = requestToken
            requestToken = ""
            window.project.answerNonZeroZ(token, false)
        }
        contentItem: Flickable {
            implicitHeight: 0
            contentWidth: width
            contentHeight: zText.implicitHeight
            boundsBehavior: Flickable.StopAtBounds
            clip: true
            ScrollBar.vertical: ScrollBar {}
            Label {
                id: zText
                objectName: "nonZeroZMessage"
                width: zDialog.availableWidth
                textFormat: Text.PlainText
                text: window.project.nonZeroZText
                wrapMode: Text.Wrap
            }
        }
        footer: DialogButtonBox {
            Button {
                objectName: "continueZButton"
                text: "Продолжить импорт"
                onClicked: {
                    const token = zDialog.requestToken
                    zDialog.requestToken = ""
                    window.project.answerNonZeroZ(token, true)
                    zDialog.close()
                }
            }
            Button {
                objectName: "cancelZButton"
                text: "Отменить импорт"
                onClicked: zDialog.close()
            }
        }
    }

    footer: Pane {
        ColumnLayout {
            anchors.fill: parent
            RowLayout {
                Layout.fillWidth: true
                Button {
                    objectName: "newProjectButton"
                    text: "Новый проект"
                    enabled: window.project.canImport
                    onClicked: window.project.newProject()
                }
                Item { Layout.fillWidth: true }
                Button {
                    objectName: "closeButton"
                    text: "Выход"
                    onClicked: window.close()
                }
            }
            Label {
                text: window.project.workflowPending && window.project.workflowPrompt.length === 0
                    ? "Ожидание завершения операции…" : window.project.operationText
                Layout.fillWidth: true
                wrapMode: Text.Wrap
            }
            Button {
                objectName: "stopOperationButton"
                text: "Остановить операцию"
                visible: window.project.operationActive
                enabled: window.project.canCancelOperation
                Layout.fillWidth: true
                onClicked: window.project.cancelOperation()
            }
        }
    }

    ScrollView {
        objectName: "projectScrollView"
        anchors.fill: parent
        anchors.margins: 16
        contentWidth: availableWidth
        clip: true

        ColumnLayout {
            width: parent.width
            spacing: 8

            Label {
                objectName: "projectStatus"
                text: window.project.hasProject ? "Текущий проект" : "Нет открытого проекта"
                font.pixelSize: 24
                Layout.fillWidth: true
            }
            Label {
                objectName: "projectPathLabel"
                text: window.project.projectName + (window.project.dirty ? " · Несохранённые изменения" : "")
                    + (window.project.hasDraft ? " · Есть неприменённый ввод" : "")
                    + "\n" + (window.project.projectPath.length > 0 ? window.project.projectPath : "Расположение ещё не выбрано")
                textFormat: Text.PlainText
                wrapMode: Text.WrapAnywhere
                Layout.fillWidth: true
            }
            Label {
                objectName: "fileMessageLabel"
                text: window.project.fileMessage
                textFormat: Text.PlainText
                visible: text.length > 0
                Layout.fillWidth: true
                wrapMode: Text.WrapAnywhere
            }

            Frame {
                objectName: "datasetCard"
                visible: window.project.hasDataset
                Layout.fillWidth: true
                ColumnLayout {
                    anchors.fill: parent
                    Label {
                        objectName: "sourceNameLabel"
                        textFormat: Text.PlainText
                        text: window.project.sourceName
                        font.bold: true
                        wrapMode: Text.WrapAnywhere
                        Layout.fillWidth: true
                    }
                    Label {
                        objectName: "pointCountLabel"
                        text: "Точек: " + window.project.pointCountText
                        wrapMode: Text.Wrap
                        Layout.fillWidth: true
                    }
                    Label {
                        objectName: "boundsLabel"
                        text: window.project.boundsText
                        wrapMode: Text.Wrap
                        Layout.fillWidth: true
                    }
                    Label {
                        objectName: "cacheStatusLabel"
                        text: window.project.cacheStatusText
                        wrapMode: Text.Wrap
                        Layout.fillWidth: true
                    }
                    Button {
                        objectName: "recheckCacheButton"
                        text: "Повторить ограниченную проверку кэша"
                        enabled: window.project.canRecheck
                        onClicked: window.project.recheckPointCache()
                    }
                    Label {
                        objectName: "datasetZLabel"
                        visible: window.project.datasetHasNonZeroZ
                        text: "Источник содержит ненулевой Z. Для обработки используются X и Y."
                        wrapMode: Text.Wrap
                        Layout.fillWidth: true
                    }
                }
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
                    enabled: window.project.canEditCell
                    placeholderText: "Размер ячейки, мм"
                    Accessible.name: "Размер ячейки в миллиметрах"
                    selectByMouse: true
                    Component.onCompleted: cellInput.text = window.project.cellDraft
                    onTextChanged: window.project.editCell(text)
                    onAccepted: window.project.applyCell(cellInput.text)
                }
                Button {
                    objectName: "applyButton"
                    text: "Применить"
                    enabled: window.project.canEditCell
                    onClicked: window.project.applyCell(cellInput.text)
                }
            }

            Label {
                objectName: "importErrorLabel"
                text: window.project.importErrorMessage
                visible: text.length > 0
                color: "#b42318"
                Layout.fillWidth: true
                wrapMode: Text.Wrap
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
                Label {
                    objectName: "importDiagnosticLabel"
                    textFormat: Text.PlainText
                    text: window.project.diagnosticText
                    visible: text.length > 0 && window.diagnosticsVisible
                    wrapMode: Text.WrapAnywhere
                    Layout.fillWidth: true
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
                    enabled: window.project.canUndo && window.project.cacheAvailable && window.project.actionsEnabled
                    onClicked: window.project.undo()
                }
                Button {
                    objectName: "redoButton"
                    text: "Повторить"
                    enabled: window.project.canRedo && window.project.cacheAvailable && window.project.actionsEnabled
                    onClicked: window.project.redo()
                }
            }

        }
    }
}
