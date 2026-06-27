import QtQuick
import QtQuick.Controls

// Solid accent-coloured action button. One per logical action group max.
Button {
    id: root
    property var theme

    implicitHeight: 34
    leftPadding: theme.s4
    rightPadding: theme.s4

    contentItem: Text {
        text: root.text
        color: root.enabled ? "#ffffff" : theme.fg4
        font.pixelSize: theme.fpBody
        font.bold: true
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }

    background: Rectangle {
        radius: theme.rSm
        color: !root.enabled ? Qt.darker(theme.accent, 2.0)
            : root.down       ? Qt.darker(theme.accent, 1.2)
            : root.hovered    ? theme.accentHover
            :                   theme.accent
        Behavior on color { ColorAnimation { duration: theme.durFast } }
    }
}
