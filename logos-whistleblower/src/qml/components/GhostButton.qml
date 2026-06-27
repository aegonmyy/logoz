import QtQuick
import QtQuick.Controls

// Outline / low-emphasis button. Use for secondary actions next to a PrimaryButton.
Button {
    id: root
    property var theme

    implicitHeight: 30
    leftPadding: theme.s3
    rightPadding: theme.s3

    contentItem: Text {
        text: root.text
        color: !root.enabled ? theme.fg4
             : root.hovered  ? theme.fg
             :                 theme.fg2
        font.pixelSize: theme.fpSm
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }

    background: Rectangle {
        radius: theme.rSm
        color: !root.enabled ? "transparent"
            : root.down      ? theme.surfaceRaised
            : root.hovered   ? theme.surfaceSubtle
            :                  "transparent"
        border.width: 1
        border.color: !root.enabled ? Qt.rgba(theme.line.r, theme.line.g, theme.line.b, 0.5)
                                    : theme.line
        Behavior on color { ColorAnimation { duration: theme.durFast } }
    }
}
