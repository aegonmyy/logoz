import QtQuick
import QtQuick.Controls

// Low-emphasis outline button for secondary / tertiary actions.
Button {
    id: root
    property var t

    implicitHeight:  28
    leftPadding:     t.sp3
    rightPadding:    t.sp3

    contentItem: Text {
        text:               root.text
        color:              !root.enabled ? t.ink4
                          : root.hovered  ? t.ink
                          :                 t.ink2
        font.pixelSize:     t.fsSm
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment:   Text.AlignVCenter
        Behavior on color { ColorAnimation { duration: t.quick } }
    }

    background: Rectangle {
        radius: t.rXs
        color: !root.enabled ? "transparent"
             : root.down     ? t.panelHigh
             : root.hovered  ? t.panelMid
             :                 "transparent"
        border.width: 1
        border.color: root.enabled ? t.rim
                                   : Qt.rgba(t.rim.r, t.rim.g, t.rim.b, 0.4)
        Behavior on color { ColorAnimation { duration: t.quick } }
    }
}
