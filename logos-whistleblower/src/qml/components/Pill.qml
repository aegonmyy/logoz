import QtQuick
import QtQuick.Layouts

// Compact status badge. tone: "neutral" | "accent" | "success" | "warning" | "danger".
// Optional pulsing dot for in-flight states.
Rectangle {
    id: root
    property var theme
    property string text: ""
    property string tone: "neutral"
    property bool pulse: false

    readonly property color toneColor:
        tone === "success" ? theme.success
      : tone === "warning" ? theme.warning
      : tone === "danger"  ? theme.danger
      : tone === "accent"  ? theme.accent
      :                      theme.muted

    implicitHeight: 22
    implicitWidth: row.implicitWidth + theme.s3 * 2
    radius: implicitHeight / 2
    color: Qt.rgba(toneColor.r, toneColor.g, toneColor.b, 0.14)
    border.color: Qt.rgba(toneColor.r, toneColor.g, toneColor.b, 0.45)
    border.width: 1

    RowLayout {
        id: row
        anchors.centerIn: parent
        spacing: theme.s2

        Rectangle {
            visible: root.pulse
            width: 6; height: 6
            radius: 3
            color: root.toneColor
            SequentialAnimation on opacity {
                running: root.pulse
                loops: Animation.Infinite
                NumberAnimation { to: 0.35; duration: 700 }
                NumberAnimation { to: 1.0;  duration: 700 }
            }
        }
        Text {
            text: root.text
            color: root.toneColor
            font.pixelSize: theme.fpLabel
            font.bold: true
            font.letterSpacing: 0.4
        }
    }
}
