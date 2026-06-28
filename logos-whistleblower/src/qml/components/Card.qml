import QtQuick

// Raised panel with rim border and medium corner radius.
Rectangle {
    property var t
    color: t.panel
    radius: t.rSm
    border.width: 1
    border.color: t.rim
}
