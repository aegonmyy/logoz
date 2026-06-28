import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Labeled text input with teal focus ring.
ColumnLayout {
    id: root

    property var    t
    property string label:           ""
    property alias  text:            tf.text
    property alias  placeholderText: tf.placeholderText
    property string hint:            ""

    spacing: t.sp1

    Text {
        text:            root.label.toUpperCase()
        color:           t.ink3
        font.pixelSize:  t.fsXs
        font.letterSpacing: 1.0
        font.bold:       true
    }

    TextField {
        id: tf
        Layout.fillWidth: true
        color:                 t.ink
        placeholderTextColor:  t.ink4
        selectionColor:        t.tealMuted
        selectedTextColor:     t.ink
        font.pixelSize:        t.fsMd

        background: Rectangle {
            color:         t.panelMid
            radius:        t.rXs
            border.width:  1
            border.color:  tf.activeFocus ? t.teal : t.rim
            Behavior on border.color { ColorAnimation { duration: t.quick } }
        }
    }

    Text {
        visible:        root.hint.length > 0
        text:           root.hint
        color:          t.ink3
        font.pixelSize: t.fsXs
    }
}
