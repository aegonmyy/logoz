import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Labeled text input with uppercase mono label and accent focus ring.
ColumnLayout {
    id: root
    property var theme
    property string label: ""
    property alias text: input.text
    property alias placeholderText: input.placeholderText
    property string errorText: ""

    spacing: theme.s1

    Text {
        text: root.label.toUpperCase()
        color: theme.fg3
        font.pixelSize: theme.fpLabel
        font.letterSpacing: 0.8
        font.bold: true
    }

    TextField {
        id: input
        Layout.fillWidth: true
        color: theme.fg
        placeholderTextColor: theme.fg4
        selectionColor: theme.accentMuted
        selectedTextColor: theme.fg
        font.pixelSize: theme.fpBody
        padding: theme.s2

        background: Rectangle {
            color: theme.surface
            radius: theme.rSm
            border.width: 1
            border.color: input.activeFocus       ? theme.accent
                       : root.errorText.length > 0 ? theme.danger
                                                   : theme.line
            Behavior on border.color { ColorAnimation { duration: theme.durFast } }
        }
    }

    Text {
        visible: root.errorText.length > 0
        text: root.errorText
        color: theme.danger
        font.pixelSize: theme.fpLabel
    }
}
