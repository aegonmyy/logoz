import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Modal form for configuring the on-chain anchor.
// Fields are pre-filled from the persisted chronicle config on open.
Dialog {
    id: root

    property var    t
    property string capabilitiesJson:  ""
    property string configJson:        "{}"
    // Set this to the publishId that triggered the dialog; callers can use it
    // to resume the anchor flow once the user saves.
    property string pendingPublishId:  ""

    signal save(string cfgJson)

    readonly property var cfg: {
        try { return JSON.parse(configJson) || {} } catch (_) { return {} }
    }
    readonly property var caps: {
        try { return JSON.parse(capabilitiesJson) || {} } catch (_) { return {} }
    }
    readonly property var missing: caps.missing_fields || []
    function needsField(name) { return missing.indexOf(name) !== -1 }

    modal:       true
    width:       500
    padding:     t.sp5
    closePolicy: Popup.CloseOnEscape | Popup.NoAutoClose

    background: Rectangle {
        color:        t.panel
        radius:       t.rSm
        border.color: t.rimStrong
        border.width: 1
    }

    onOpened: {
        progField.text   = cfg.program_id        || ""
        seqField.text    = cfg.sequencer_url     || ""
        walletField.text = cfg.wallet_home       || ""
        acctField.text   = cfg.signer_account_id || ""
    }

    contentItem: ColumnLayout {
        spacing: t.sp3

        Text {
            text:           "Anchor configuration"
            color:          t.ink
            font.pixelSize: t.fsLg
            font.bold:      true
        }

        Text {
            text: "Chronicle writes each publish's (cid, metadata_hash, timestamp) " +
                  "to your deployed chronicle-registry SPEL program. " +
                  "Settings are saved locally by chronicle."
            color:          t.ink3
            font.pixelSize: t.fsSm
            wrapMode:       Text.Wrap
            Layout.fillWidth: true
        }

        Field {
            id: progField
            Layout.fillWidth: true
            t:     root.t
            label: root.needsField("program_id") ? "Program ID  (required)" : "Program ID"
            placeholderText: "32-byte hex  — from `spel program-id --format hex`"
        }
        Field {
            id: seqField
            Layout.fillWidth: true
            t:     root.t
            label: "Sequencer URL"
            placeholderText: "http://127.0.0.1:3040"
        }
        Field {
            id: walletField
            Layout.fillWidth: true
            t:     root.t
            label: "Wallet home"
            placeholderText: "Path to spel-framework wallet directory"
        }
        Field {
            id: acctField
            Layout.fillWidth: true
            t:     root.t
            label: "Signer account ID"
            placeholderText: "Base58 account id"
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: t.sp2
            spacing: t.sp2
            Item { Layout.fillWidth: true }
            GhostButton {
                t: root.t
                text: "Cancel"
                onClicked: root.close()
            }
            PrimaryButton {
                t: root.t
                text: "Save"
                onClicked: {
                    root.save(JSON.stringify({
                        program_id:        progField.text.trim(),
                        sequencer_url:     seqField.text.trim(),
                        wallet_home:       walletField.text.trim(),
                        signer_account_id: acctField.text.trim(),
                    }))
                    root.close()
                }
            }
        }
    }
}
