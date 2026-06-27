import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

import "components" as Comp

// Whistleblower main screen. Root is a Rectangle so the app paints its own
// canvas with the Theme's bg color instead of inheriting the host's window
// chrome colour.
Rectangle {
    id: root
    color: theme.bg
    // Exposed so Repeater delegates can reference the theme without their
    // own `theme:` binding shadowing the outer id.
    readonly property var palette: theme

    // ── Backend wiring ───────────────────────────────────────────────────────
    readonly property var backend: logos.module("whistleblower")
    readonly property bool connected: backend !== null && backend.state === 2
    readonly property string status: backend ? backend.status : ""
    readonly property bool busy: backend ? backend.busy : false
    readonly property string cid: backend ? backend.cid : ""
    readonly property string metadataHash: backend ? backend.metadataHash : ""
    readonly property string lastError: backend ? backend.lastError : ""
    readonly property bool deliveryReady: backend ? backend.deliveryReady : false
    readonly property string publishedRecordsJson: backend ? backend.publishedRecordsJson : ""

    readonly property var publishedRecords: {
        if (!publishedRecordsJson) return []
        try { return JSON.parse(publishedRecordsJson) } catch (e) { return [] }
    }

    // ── Anchor state ────────────────────────────────────────────────────────
    readonly property string anchorCapabilitiesJson: backend ? backend.anchorCapabilitiesJson : ""
    readonly property string anchorConfigJson:       backend ? backend.anchorConfigJson : ""
    readonly property string anchorsJson:            backend ? backend.anchorsJson : ""
    readonly property var anchorCaps: {
        if (!anchorCapabilitiesJson) return { configured: false, missing_fields: [] }
        try { return JSON.parse(anchorCapabilitiesJson) } catch (e) { return { configured: false, missing_fields: [] } }
    }
    readonly property bool anchorConfigured: !!anchorCaps.configured
    // anchorsJson is keyed by CID (matches on-chain identity, lets two
    // publishes of the same document share anchor state).
    readonly property var anchorsByCid: {
        if (!anchorsJson) return {}
        try { return JSON.parse(anchorsJson) || {} } catch (e) { return {} }
    }
    function anchorStateFor(cid) {
        if (!cid) return null
        return anchorsByCid[cid] || null
    }
    function anchorButtonLabel(cid) {
        var s = anchorStateFor(cid)
        if (!s) return "Anchor"
        if (s.state === "confirmed") return "Anchored ✓"
        if (s.state === "failed") return "Retry"
        return "Anchor"
    }

    Theme { id: theme }

    function connectionPillTone() {
        if (!connected || !deliveryReady) return "warning"
        return "success"
    }
    function connectionPillText() {
        if (!connected)     return "Connecting…"
        if (!deliveryReady) return "Delivery offline"
        return "Connected"
    }
    function statusPillTone(s) {
        if (s === "broadcast_sent")  return "success"
        if (s === "error")           return "danger"
        if (s === "idle" || s === "") return "neutral"
        return "warning"
    }

    FileDialog {
        id: filePicker
        title: "Choose a file to publish"
        onAccepted: {
            // selectedFile is a file:// URL — strip the scheme for chronicle.
            var u = filePicker.selectedFile.toString()
            pathField.text = u.replace(/^file:\/\//, "")
        }
    }

    MessageDialog {
        id: clearConfirm
        title: "Clear publish history?"
        text: "Remove all " + root.publishedRecords.length +
              " local publish record(s)? Files already uploaded to Storage and " +
              "broadcasts already sent will not be rolled back."
        buttons: MessageDialog.Yes | MessageDialog.Cancel
        onAccepted: backend.clearHistory()
    }

    Comp.AnchorConfigDialog {
        id: anchorConfig
        theme: theme
        anchors.centerIn: parent
        capabilitiesJson: root.anchorCapabilitiesJson
        // configJson from chronicle is wrapped: {"ok": true, "config": {...}}
        configJson: {
            try {
                var w = JSON.parse(root.anchorConfigJson || "{}")
                return JSON.stringify(w.config || {})
            } catch (e) { return "{}" }
        }
        onSave: function(cfgJson) { backend.setAnchorConfig(cfgJson) }
    }

    function handleAnchorClick(publishId) {
        if (!root.anchorConfigured) {
            anchorConfig.pendingPublishId = publishId
            anchorConfig.open()
            return
        }
        backend.anchorPublished(publishId)
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: theme.s5
        spacing: theme.s4

        // ── Header ───────────────────────────────────────────────────────────
        RowLayout {
            Layout.fillWidth: true
            spacing: theme.s3

            ColumnLayout {
                spacing: 2
                Text {
                    text: "Whistleblower"
                    color: theme.fg
                    font.pixelSize: theme.fpHero
                    font.bold: true
                }
                Text {
                    text: "Censorship-resistant document publishing"
                    color: theme.fg3
                    font.pixelSize: theme.fpSm
                }
            }
            Item { Layout.fillWidth: true }
            Comp.Pill {
                theme: theme
                text: root.connectionPillText()
                tone: root.connectionPillTone()
                pulse: !root.connected || !root.deliveryReady
                Layout.alignment: Qt.AlignVCenter
            }
            Comp.GhostButton {
                theme: root.palette
                text: "Anchor settings"
                Layout.alignment: Qt.AlignVCenter
                onClicked: {
                    anchorConfig.pendingPublishId = ""
                    anchorConfig.open()
                }
            }
        }

        // ── Document form ────────────────────────────────────────────────────
        Comp.Card {
            theme: theme
            Layout.fillWidth: true
            Layout.preferredHeight: formCol.implicitHeight + theme.s4 * 2

            ColumnLayout {
                id: formCol
                anchors.fill: parent
                anchors.margins: theme.s4
                spacing: theme.s3

                Text {
                    text: "Document"
                    color: theme.fg
                    font.pixelSize: theme.fpLg
                    font.bold: true
                }

                RowLayout {
                    spacing: theme.s2
                    Layout.fillWidth: true
                    Comp.Field {
                        id: pathField
                        Layout.fillWidth: true
                        theme: theme
                        label: "File path"
                        placeholderText: "/absolute/path/to/file"
                        enabled: !root.busy
                    }
                    Comp.GhostButton {
                        theme: theme
                        text: "Browse…"
                        Layout.alignment: Qt.AlignBottom
                        enabled: !root.busy
                        onClicked: filePicker.open()
                    }
                }

                Comp.Field {
                    id: titleField
                    Layout.fillWidth: true
                    theme: theme
                    label: "Title"
                    placeholderText: "Public title"
                    enabled: !root.busy
                }
                Comp.Field {
                    id: descField
                    Layout.fillWidth: true
                    theme: theme
                    label: "Description"
                    placeholderText: "(optional)"
                    enabled: !root.busy
                }
                Comp.Field {
                    id: tagsField
                    Layout.fillWidth: true
                    theme: theme
                    label: "Tags (CSV)"
                    placeholderText: "evidence, internal, draft"
                    enabled: !root.busy
                }

                RowLayout {
                    Layout.fillWidth: true
                    Layout.topMargin: theme.s1
                    spacing: theme.s2
                    Item { Layout.fillWidth: true }
                    Comp.GhostButton {
                        theme: theme
                        text: "Reconnect"
                        visible: !root.deliveryReady
                        enabled: root.connected && !root.busy
                        onClicked: backend.startBroadcaster()
                    }
                    Comp.PrimaryButton {
                        theme: theme
                        text: root.busy ? "Publishing…" : "Publish"
                        enabled: root.connected && !root.busy &&
                                 pathField.text.length > 0 && titleField.text.length > 0
                        onClicked: {
                            // Empty content type → plugin auto-detects via QMimeDatabase
                            logos.watch(backend.publish(
                                pathField.text, "",
                                titleField.text, descField.text, tagsField.text
                            ), function() {}, function(err) {
                                console.warn("publish call failed:", err)
                            })
                        }
                    }
                }
            }
        }

        // ── Current-publish status ──────────────────────────────────────────
        Comp.Card {
            theme: theme
            Layout.fillWidth: true
            Layout.preferredHeight: statusCol.implicitHeight + theme.s3 * 2
            visible: root.status.length > 0 || root.cid.length > 0

            ColumnLayout {
                id: statusCol
                anchors.fill: parent
                anchors.margins: theme.s3
                spacing: theme.s2

                RowLayout {
                    spacing: theme.s2
                    Text {
                        text: "STATUS"
                        color: theme.fg3
                        font.pixelSize: theme.fpLabel
                        font.letterSpacing: 0.8
                        font.bold: true
                    }
                    Comp.Pill {
                        theme: theme
                        text: root.status === "" ? "idle" : root.status
                        tone: root.statusPillTone(root.status)
                        pulse: root.busy
                    }
                    BusyIndicator {
                        running: root.busy
                        visible: root.busy
                        Layout.preferredHeight: 16
                        Layout.preferredWidth: 16
                    }
                    Item { Layout.fillWidth: true }
                }

                Text {
                    visible: root.cid !== ""
                    text: "CID   " + root.cid
                    color: theme.success
                    font.pixelSize: theme.fpSm
                    font.family: "monospace"
                    Layout.fillWidth: true
                    wrapMode: Text.WrapAnywhere
                }
                Text {
                    visible: root.metadataHash !== ""
                    text: "HASH  " + root.metadataHash
                    color: theme.fg3
                    font.pixelSize: theme.fpLabel
                    font.family: "monospace"
                    Layout.fillWidth: true
                    wrapMode: Text.WrapAnywhere
                }
            }
        }

        // ── Error toast (auto-collapses when no error) ──────────────────────
        Comp.ToastBanner {
            theme: theme
            Layout.fillWidth: true
            message: root.lastError
            tone: "danger"
        }

        // ── History header ──────────────────────────────────────────────────
        RowLayout {
            Layout.fillWidth: true
            spacing: theme.s2
            Text {
                text: "History"
                color: theme.fg
                font.pixelSize: theme.fpLg
                font.bold: true
            }
            Text {
                text: "(" + root.publishedRecords.length + ")"
                color: theme.fg3
                font.pixelSize: theme.fpSm
            }
            Item { Layout.fillWidth: true }
            Comp.GhostButton {
                theme: theme
                text: "Refresh"
                enabled: root.connected
                onClicked: backend.refreshPublishedList()
            }
            Comp.GhostButton {
                theme: theme
                text: "Clear"
                enabled: root.connected && !root.busy && root.publishedRecords.length > 0
                onClicked: clearConfirm.open()
            }
        }

        // ── History list ────────────────────────────────────────────────────
        Comp.Card {
            theme: theme
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 140

            ListView {
                anchors.fill: parent
                anchors.margins: theme.s2
                clip: true
                model: root.publishedRecords
                spacing: theme.s1

                delegate: Rectangle {
                    id: row
                    width: ListView.view.width
                    height: rowCol.implicitHeight + theme.s3
                    color: index % 2 === 0 ? theme.surfaceSubtle : "transparent"
                    radius: theme.rSm

                    readonly property color statusColor:
                        modelData.status === "broadcast_sent" ? theme.success
                      : modelData.status === "error"          ? theme.danger
                      :                                         theme.warning

                    ColumnLayout {
                        id: rowCol
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.leftMargin: theme.s3
                        anchors.rightMargin: theme.s3
                        spacing: 2

                        RowLayout {
                            spacing: theme.s2
                            Layout.fillWidth: true
                            Text {
                                Layout.fillWidth: true
                                text: modelData.title || "(untitled)"
                                color: theme.fg
                                font.pixelSize: theme.fpBody
                                font.bold: true
                                elide: Text.ElideRight
                            }
                            Text {
                                text: (modelData.status || "").toUpperCase()
                                color: row.statusColor
                                font.pixelSize: theme.fpLabel
                                font.bold: true
                                font.letterSpacing: 0.4
                            }
                            Comp.GhostButton {
                                visible: modelData.status === "broadcast_sent"
                                theme: root.palette
                                text: root.anchorButtonLabel(modelData.cid)
                                enabled: {
                                    var s = root.anchorStateFor(modelData.cid)
                                    return !(s && s.state === "submitting")
                                }
                                onClicked: root.handleAnchorClick(modelData.publish_id)
                            }
                        }
                        Text {
                            visible: !!modelData.cid
                            text: "CID  " + (modelData.cid || "")
                            color: theme.fg3
                            font.pixelSize: theme.fpLabel
                            font.family: "monospace"
                            Layout.fillWidth: true
                            elide: Text.ElideMiddle
                        }
                        Text {
                            visible: !!modelData.error && modelData.status === "error"
                            text: (modelData.code ? modelData.code + ": " : "") + (modelData.error || "")
                            color: theme.danger
                            font.pixelSize: theme.fpLabel
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                        }
                    }
                }

                Text {
                    anchors.centerIn: parent
                    visible: root.publishedRecords.length === 0
                    text: "No publishes yet"
                    color: theme.fg4
                    font.pixelSize: theme.fpSm
                }
            }
        }
    }
}
