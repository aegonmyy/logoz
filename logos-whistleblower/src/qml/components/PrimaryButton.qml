import QtQuick
import QtQuick.Controls

// Teal-filled primary action button.
Button {
    id: root
    property var t

    implicitHeight:  34
    leftPadding:     t.sp5
    rightPadding:    t.sp5

    contentItem: Text {
        text:               root.text
        color:              root.enabled ? t.canvas : t.ink4
        font.pixelSize:     t.fsMd
        font.bold:          true
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment:   Text.AlignVCenter
    }

    background: Rectangle {
        radius: t.rXs
        color: !root.enabled ? Qt.rgba(t.teal.r, t.teal.g, t.teal.b, 0.25)
             : root.down     ? t.tealMuted
             : root.hovered  ? t.tealHover
             :                 t.teal
        Behavior on color { ColorAnimation { duration: t.quick } }
    }
}
