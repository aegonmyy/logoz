import QtQuick
import QtQuick.Layouts

// Slide-in error/info banner. Collapses to height 0 when message is empty.
Rectangle {
    id: root
    property var theme
    property string message: ""
    property string tone: "danger"

    readonly property color toneColor:
        tone === "success" ? theme.success
      : tone === "warning" ? theme.warning
      : tone === "danger"  ? theme.danger
      :                      theme.accent

    height: message.length > 0 ? content.implicitHeight + theme.s3 * 2 : 0
    visible: height > 0
    Behavior on height { NumberAnimation { duration: theme.durBase; easing.type: Easing.OutCubic } }

    color: Qt.rgba(toneColor.r, toneColor.g, toneColor.b, 0.12)
    border.color: toneColor
    border.width: 1
    radius: theme.rSm
    clip: true

    RowLayout {
        id: content
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        anchors.leftMargin: theme.s3
        anchors.rightMargin: theme.s3
        spacing: theme.s2
        Text {
            Layout.fillWidth: true
            text: root.message
            color: theme.fg
            font.pixelSize: theme.fpBody
            wrapMode: Text.Wrap
        }
    }
}
