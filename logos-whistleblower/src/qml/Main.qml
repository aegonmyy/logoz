import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

import "components" as C

Rectangle {
    id: root
    color: t.canvas

    // ── Theme token object ────────────────────────────────────────────────────
    Theme { id: t }

    // ── Backend wiring ────────────────────────────────────────────────────────
    readonly property var    bk:           logos.module("whistleblower")
    readonly property bool   linked:       bk !== null && bk.state === 2

    readonly property string phase:        bk ? bk.phase        : ""
    readonly property bool   working:      bk ? bk.working      : false
    readonly property string contentId:    bk ? bk.contentId    : ""
    readonly property string docHash:      bk ? bk.docHash      : ""
    readonly property string lastErr:      bk ? bk.lastErr      : ""
    readonly property bool   networkReady: bk ? bk.networkReady : false
    readonly property string historyJson:  bk ? bk.historyJson  : "[]"

    readonly property string anchorCapsJson: bk ? bk.anchorCapsJson : ""
    readonly property string anchorCfgJson:  bk ? bk.anchorCfgJson  : ""
    readonly property string anchorsMapJson: bk ? bk.anchorsMapJson  : "{}"

    readonly property var history: {
        try { return JSON.parse(historyJson) || [] } catch (_) { return [] }
    }
    readonly property var anchorCaps: {
        try { return JSON.parse(anchorCapsJson) || {} } catch (_) { return {} }
    }
    readonly property bool anchorReady: !!anchorCaps.configured

    readonly property var anchorsMap: {
        try { return JSON.parse(anchorsMapJson) || {} } catch (_) { return {} }
    }

    function anchorState(cid) { return cid ? (anchorsMap[cid] || null) : null }

    function anchorLabel(cid) {
        var s = anchorState(cid)
        if (!s)                      return "Anchor"
        if (s.state === "confirmed") return "Anchored ✓"
        if (s.state === "failed")    return "Retry anchor"
        return "Anchor"
    }

    function onAnchorClicked(publishId) {
        if (!root.anchorReady) {
            cfgDialog.pendingPublishId = publishId
            cfgDialog.open()
        } else {
            bk.anchorJob(publishId)
        }
    }

    // ── Connection / phase helpers ────────────────────────────────────────────
    function connTone()  {
        if (!linked || !networkReady) return "warn"
        return "ok"
    }
    function connLabel() {
        if (!linked)      return "Connecting…"
        if (!networkReady) return "Delivery offline"
        return "Live"
    }
    function phaseTone(p) {
        if (p === "done")  return "ok"
        if (p === "error") return "err"
        if (p === "idle" || p === "") return "neutral"
        return "warn"
    }

    // ── Dialogs ───────────────────────────────────────────────────────────────
    FileDialog {
        id: filePicker
        title: "Select file to publish"
        onAccepted: {
            var u = filePicker.selectedFile.toString()
            pathIn.text = u.replace(/^file:\/\//, "")
        }
    }

    MessageDialog {
        id: clearDlg
        title: "Discard history?"
        text: "Remove all " + root.history.length + " record(s) from the local ledger? " +
              "Uploads already on Storage and broadcasts already sent are not rolled back."
        buttons: MessageDialog.Yes | MessageDialog.Cancel
        onAccepted: bk.discardHistory()
    }

    C.AnchorConfigDialog {
        id: cfgDialog
        t:               t
        capabilitiesJson: root.anchorCapsJson
        configJson: {
            try {
                var w = JSON.parse(root.anchorCfgJson || "{}")
                return JSON.stringify(w.config || {})
            } catch (_) { return "{}" }
        }
        anchors.centerIn: parent
        onSave: function(cfg) { bk.applyAnchorConfig(cfg) }
    }

    // ── Root layout ───────────────────────────────────────────────────────────
    ColumnLayout {
        anchors.fill:    parent
        anchors.margins: t.sp5
        spacing:         t.sp4

        // ── Header row ────────────────────────────────────────────────────────
        RowLayout {
            Layout.fillWidth: true
            spacing: t.sp3

            ColumnLayout {
                spacing: 2
                Text {
                    text:           "Whistleblower"
                    color:          t.ink
                    font.pixelSize: t.fsTitle
                    font.bold:      true
                }
                Text {
                    text:           "Censorship-resistant document publishing on Logos"
                    color:          t.ink3
                    font.pixelSize: t.fsSm
                }
            }

            Item { Layout.fillWidth: true }

            C.Pill {
                t:     t
                label: root.connLabel()
                tone:  root.connTone()
                blink: !root.linked || !root.networkReady
                Layout.alignment: Qt.AlignVCenter
            }

            C.GhostButton {
                t:    t
                text: "Anchor settings"
                Layout.alignment: Qt.AlignVCenter
                onClicked: {
                    cfgDialog.pendingPublishId = ""
                    cfgDialog.open()
                }
            }
        }

        // ── Publish form ──────────────────────────────────────────────────────
        C.Card {
            t: t
            Layout.fillWidth: true
            Layout.preferredHeight: formLayout.implicitHeight + t.sp5 * 2

            ColumnLayout {
                id: formLayout
                anchors.fill:    parent
                anchors.margins: t.sp4
                spacing:         t.sp3

                Text {
                    text:           "Publish document"
                    color:          t.ink
                    font.pixelSize: t.fsLg
                    font.bold:      true
                }

                RowLayout {
                    spacing: t.sp2
                    Layout.fillWidth: true

                    C.Field {
                        id: pathIn
                        Layout.fillWidth: true
                        t:               t
                        label:           "File path"
                        placeholderText: "/absolute/path/to/document"
                        enabled:         !root.working
                    }
                    C.GhostButton {
                        t:        t
                        text:     "Browse…"
                        enabled:  !root.working
                        Layout.alignment: Qt.AlignBottom
                        onClicked: filePicker.open()
                    }
                }

                C.Field {
                    id: titleIn
                    Layout.fillWidth: true
                    t:               t
                    label:           "Title"
                    placeholderText: "Human-readable title (required)"
                    enabled:         !root.working
                }

                C.Field {
                    id: descIn
                    Layout.fillWidth: true
                    t:               t
                    label:           "Description"
                    placeholderText: "Optional summary"
                    enabled:         !root.working
                }

                C.Field {
                    id: tagsIn
                    Layout.fillWidth: true
                    t:               t
                    label:           "Tags  (comma-separated)"
                    placeholderText: "leak, finance, 2025"
                    enabled:         !root.working
                }

                RowLayout {
                    Layout.fillWidth: true
                    Layout.topMargin: t.sp1
                    spacing: t.sp2

                    Item { Layout.fillWidth: true }

                    C.GhostButton {
                        t:       t
                        text:    "Reconnect"
                        visible: !root.networkReady
                        enabled: root.linked && !root.working
                        onClicked: bk.connectDelivery()
                    }

                    C.PrimaryButton {
                        t:       t
                        text:    root.working ? "Publishing…" : "Publish"
                        enabled: root.linked && !root.working &&
                                 pathIn.text.length > 0 &&
                                 titleIn.text.length > 0
                        onClicked: {
                            bk.submitDocument(pathIn.text, "",
                                              titleIn.text, descIn.text, tagsIn.text)
                        }
                    }
                }
            }
        }

        // ── Phase / result strip ──────────────────────────────────────────────
        C.Card {
            t: t
            Layout.fillWidth: true
            Layout.preferredHeight: phaseRow.implicitHeight + t.sp3 * 2
            visible: root.phase.length > 0 || root.contentId.length > 0

            ColumnLayout {
                id: phaseRow
                anchors.fill:    parent
                anchors.margins: t.sp3
                spacing:         t.sp2

                RowLayout {
                    spacing: t.sp2

                    Text {
                        text:               "STATUS"
                        color:              t.ink3
                        font.pixelSize:     t.fsXs
                        font.letterSpacing: 1.0
                        font.bold:          true
                    }
                    C.Pill {
                        t:     t
                        label: root.phase === "" ? "idle" : root.phase.toUpperCase()
                        tone:  root.phaseTone(root.phase)
                        blink: root.working
                    }
                    BusyIndicator {
                        running: root.working
                        visible: root.working
                        Layout.preferredWidth:  14
                        Layout.preferredHeight: 14
                    }
                    Item { Layout.fillWidth: true }
                }

                Text {
                    visible:        root.contentId !== ""
                    text:           "CID   " + root.contentId
                    color:          t.ok
                    font.pixelSize: t.fsSm
                    font.family:    "monospace"
                    Layout.fillWidth: true
                    wrapMode:       Text.WrapAnywhere
                }
                Text {
                    visible:        root.docHash !== ""
                    text:           "HASH  " + root.docHash
                    color:          t.ink3
                    font.pixelSize: t.fsXs
                    font.family:    "monospace"
                    Layout.fillWidth: true
                    wrapMode:       Text.WrapAnywhere
                }
            }
        }

        // ── Error banner ──────────────────────────────────────────────────────
        C.ToastBanner {
            t:    t
            msg:  root.lastErr
            tone: "err"
            Layout.fillWidth: true
        }

        // ── History header ────────────────────────────────────────────────────
        RowLayout {
            Layout.fillWidth: true
            spacing: t.sp2

            Text {
                text:           "History"
                color:          t.ink
                font.pixelSize: t.fsLg
                font.bold:      true
            }
            Text {
                text:           "(" + root.history.length + ")"
                color:          t.ink3
                font.pixelSize: t.fsSm
            }
            Item { Layout.fillWidth: true }

            C.GhostButton {
                t:        t
                text:     "Refresh"
                enabled:  root.linked
                onClicked: bk.reloadHistory()
            }
            C.GhostButton {
                t:        t
                text:     "Clear"
                enabled:  root.linked && !root.working && root.history.length > 0
                onClicked: clearDlg.open()
            }
        }

        // ── History list ──────────────────────────────────────────────────────
        C.Card {
            t: t
            Layout.fillWidth:  true
            Layout.fillHeight: true
            Layout.minimumHeight: 120

            ListView {
                anchors.fill:    parent
                anchors.margins: t.sp2
                clip:            true
                model:           root.history
                spacing:         t.sp1

                delegate: Rectangle {
                    id: row
                    width:  ListView.view.width
                    height: rowInner.implicitHeight + t.sp3
                    color:  index % 2 === 0 ? t.panelMid : "transparent"
                    radius: t.rXs

                    readonly property color rowStatusColor:
                        modelData.status === "done"  ? t.ok
                      : modelData.status === "error" ? t.err
                      :                                t.warn

                    ColumnLayout {
                        id: rowInner
                        anchors {
                            left:           parent.left
                            right:          parent.right
                            verticalCenter: parent.verticalCenter
                            leftMargin:     t.sp3
                            rightMargin:    t.sp3
                        }
                        spacing: 2

                        RowLayout {
                            spacing: t.sp2
                            Layout.fillWidth: true

                            Text {
                                Layout.fillWidth: true
                                text:            modelData.title || "(untitled)"
                                color:           t.ink
                                font.pixelSize:  t.fsMd
                                font.bold:       true
                                elide:           Text.ElideRight
                            }
                            Text {
                                text:            (modelData.status || "").toUpperCase()
                                color:           row.rowStatusColor
                                font.pixelSize:  t.fsXs
                                font.bold:       true
                                font.letterSpacing: 0.5
                            }
                            C.GhostButton {
                                visible: modelData.status === "done"
                                t:       t
                                text:    root.anchorLabel(modelData.cid)
                                enabled: {
                                    var s = root.anchorState(modelData.cid)
                                    return !(s && s.state === "submitting")
                                }
                                onClicked: root.onAnchorClicked(modelData.publish_id)
                            }
                        }

                        Text {
                            visible:        !!modelData.cid
                            text:           "CID  " + (modelData.cid || "")
                            color:          t.ink3
                            font.pixelSize: t.fsXs
                            font.family:    "monospace"
                            Layout.fillWidth: true
                            elide:          Text.ElideMiddle
                        }

                        Text {
                            visible:        modelData.status === "error" && !!modelData.error
                            text:           (modelData.code ? modelData.code + ": " : "") +
                                            (modelData.error || "")
                            color:          t.err
                            font.pixelSize: t.fsXs
                            Layout.fillWidth: true
                            wrapMode:       Text.Wrap
                        }
                    }
                }

                Text {
                    anchors.centerIn: parent
                    visible:        root.history.length === 0
                    text:           "No publishes yet"
                    color:          t.ink4
                    font.pixelSize: t.fsSm
                }
            }
        }
    }
}
