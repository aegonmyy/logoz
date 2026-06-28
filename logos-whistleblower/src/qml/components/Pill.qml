import QtQuick
import QtQuick.Layouts

// Compact status badge. tone: "ok" | "warn" | "err" | "teal" | "neutral"
// Set blink: true to pulse the indicator dot for in-flight states.
Rectangle {
    id: root

    property var    t
    property string label:  ""
    property string tone:   "neutral"
    property bool   blink:  false

    readonly property color toneCol:
        tone === "ok"      ? t.ok
      : tone === "warn"    ? t.warn
      : tone === "err"     ? t.err
      : tone === "teal"    ? t.teal
      :                      t.neutral

    implicitHeight: 20
    implicitWidth:  row.implicitWidth + t.sp3 * 2
    radius:         implicitHeight / 2
    color:          Qt.rgba(toneCol.r, toneCol.g, toneCol.b, 0.12)
    border.color:   Qt.rgba(toneCol.r, toneCol.g, toneCol.b, 0.40)
    border.width:   1

    RowLayout {
        id: row
        anchors.centerIn: parent
        spacing: t.sp2

        Rectangle {
            visible: root.blink
            width: 5; height: 5
            radius: 3
            color: root.toneCol

            SequentialAnimation on opacity {
                running: root.blink
                loops:   Animation.Infinite
                NumberAnimation { to: 0.2;  duration: 600 }
                NumberAnimation { to: 1.0;  duration: 600 }
            }
        }

        Text {
            text:            root.label
            color:           root.toneCol
            font.pixelSize:  t.fsXs
            font.bold:       true
            font.letterSpacing: 0.5
        }
    }
}
