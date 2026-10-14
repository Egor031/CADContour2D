pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import CADContour2D
import CADContour2D.Verification

Main {
    id: root
    required property OperationProbe probe
    title: "CADContour2D — проверка фоновых операций"
    developmentControls: Component {
        ColumnLayout {
            spacing: 4
            Label {
                text: "Проверочный сценарий · данные проекта не создаются"
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }
            Button {
                objectName: "probeKnownButton"
                text: "Проверить фоновую операцию"
                enabled: root.probe.canStart
                onClicked: root.probe.startKnown()
            }
            RowLayout {
                Button {
                    text: "Неизвестный объём"
                    enabled: root.probe.canStart
                    onClicked: root.probe.startUnknown()
                }
                Button {
                    text: "Проверить ошибку"
                    enabled: root.probe.canStart
                    onClicked: root.probe.startFailure()
                }
            }
        }
    }
}
