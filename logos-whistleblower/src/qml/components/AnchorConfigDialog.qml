import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Modal config form for the on-chain anchor settings. Sibling components
// (Field, PrimaryButton, GhostButton) are in the same dir and auto-resolve.
Dialog {
    id: root
    property var theme
    // Last `anchorCapabilitiesJson` payload from chronicle (string of JSON).
    property string capabilitiesJson: ""
    // Last `getAnchorConfigJson().config` payload (string of JSON).
    property string configJson: "{}"
    // PublishId the user clicked on before we opened — exposed so callers can
    // resume the anchor flow after a successful save.
    property string pendingPublishId: ""

    signal save(string cfgJson)

    readonly property var parsedConfig: {
        try { return JSON.parse(configJson) || {} } catch (e) { return {} }
    }
    readonly property var missingFields: {
        try {
            var caps = JSON.parse(capabilitiesJson)
            return caps.missing_fields || []
        } catch (e) { return [] }
    }
    function isMissing(name) { return missingFields.indexOf(name) !== -1 }

    modal: true
    width: 520
    padding: theme.s4
    closePolicy: Popup.CloseOnEscape | Popup.NoAutoClose

    background: Rectangle {
        color: theme.surface
        radius: theme.rMd
        border.color: theme.lineStrong
        border.width: 1
    }

    onOpened: {
        var c = parsedConfig
        programIdField.text       = c.program_id        || ""
        sequencerField.text       = c.sequencer_url     || ""
        walletField.text          = c.wallet_home       || ""
        signerField.text          = c.signer_account_id || ""
    }

    contentItem: ColumnLayout {
        spacing: theme.s3

        Text {
            text: "Anchor settings"
            color: theme.fg
            font.pixelSize: theme.fpLg
            font.bold: true
        }
        Text {
            text: "On-chain anchoring writes a publish's (cid, metadata_hash, timestamp) " +
                  "to a chronicle-registry deployment. Fill these once; chronicle " +
                  "persists them locally."
            color: theme.fg3
            font.pixelSize: theme.fpSm
            wrapMode: Text.Wrap
            Layout.fillWidth: true
        }

        Field {
            id: programIdField
            Layout.fillWidth: true
            theme: root.theme
            label: "Program ID (required)"
            placeholderText: "32-byte hex from `spel deploy` output"
            errorText: root.isMissing("program_id") && programIdField.text.trim() === "" ? "Required" : ""
        }
        Field {
            id: sequencerField
            Layout.fillWidth: true
            theme: root.theme
            label: "Sequencer URL"
            placeholderText: "http://127.0.0.1:3040"
            errorText: root.isMissing("sequencer_url") && sequencerField.text.trim() === "" ? "Required" : ""
        }
        Field {
            id: walletField
            Layout.fillWidth: true
            theme: root.theme
            label: "Wallet home"
            placeholderText: "Path to spel-framework wallet directory"
            errorText: root.isMissing("wallet_home") && walletField.text.trim() === "" ? "Required" : ""
        }
        Field {
            id: signerField
            Layout.fillWidth: true
            theme: root.theme
            label: "Signer account ID"
            placeholderText: "Base58 account id"
            errorText: root.isMissing("signer_account_id") && signerField.text.trim() === "" ? "Required" : ""
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: theme.s2
            spacing: theme.s2
            Item { Layout.fillWidth: true }
            GhostButton {
                theme: root.theme
                text: "Cancel"
                onClicked: root.close()
            }
            PrimaryButton {
                theme: root.theme
                text: "Save"
                onClicked: {
                    var payload = {
                        program_id:        programIdField.text.trim(),
                        sequencer_url:     sequencerField.text.trim(),
                        wallet_home:       walletField.text.trim(),
                        signer_account_id: signerField.text.trim()
                    }
                    root.save(JSON.stringify(payload))
                    root.close()
                }
            }
        }
    }
}
