import QtQuick
import QtQuick.Layouts

// Dismissible inline alert. Collapses to zero height when msg is empty.
Rectangle {
    id: root

    property var    t
    property string msg:  ""
    property string tone: "err"

    readonly property color toneCol:
        tone === "ok"   ? t.ok
      : tone === "warn" ? t.warn
      : tone === "err"  ? t.err
      :                   t.teal

    height:  msg.length > 0 ? inner.implicitHeight + t.sp3 * 2 : 0
    visible: height > 0
    clip:    true

    Behavior on height { NumberAnimation { duration: t.smooth; easing.type: Easing.OutQuad } }

    color:        Qt.rgba(toneCol.r, toneCol.g, toneCol.b, 0.10)
    border.color: Qt.rgba(toneCol.r, toneCol.g, toneCol.b, 0.60)
    border.width: 1
    radius:       t.rXs

    RowLayout {
        id: inner
        anchors {
            left:            parent.left
            right:           parent.right
            verticalCenter:  parent.verticalCenter
            leftMargin:      t.sp3
            rightMargin:     t.sp3
        }
        spacing: t.sp2

        Rectangle {
            width: 3; height: inner.implicitHeight
            radius: 2
            color: root.toneCol
        }

        Text {
            Layout.fillWidth: true
            text:            root.msg
            color:           t.ink
            font.pixelSize:  t.fsMd
            wrapMode:        Text.Wrap
        }
    }
}
