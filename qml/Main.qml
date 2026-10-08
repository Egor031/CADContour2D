import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: window
    width: 640
    height: 360
    minimumWidth: 400
    minimumHeight: 240
    visible: true
    title: "CADContour2D"

    ColumnLayout {
        anchors.centerIn: parent
        spacing: 20

        Label {
            text: "Каркас приложения работает"
            font.pixelSize: 24
            Layout.alignment: Qt.AlignHCenter
        }

        Button {
            text: "Закрыть"
            Layout.alignment: Qt.AlignHCenter
            onClicked: window.close()
        }
    }
}
