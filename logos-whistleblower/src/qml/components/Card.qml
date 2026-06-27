import QtQuick

// Surface container with subtle border. Children anchor themselves inside;
// pass `Layout.preferredHeight` based on their implicit size + margins.
Rectangle {
    property var theme
    color: theme.surface
    radius: theme.rMd
    border.width: 1
    border.color: theme.line
}
