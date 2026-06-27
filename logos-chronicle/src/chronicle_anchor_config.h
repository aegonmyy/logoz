#ifndef CHRONICLE_ANCHOR_CONFIG_H
#define CHRONICLE_ANCHOR_CONFIG_H

#include <QJsonObject>
#include <QString>
#include <QStringList>

// Settings for the optional on-chain anchor pipeline. Persisted to a JSON file
// under QStandardPaths::AppDataLocation. The actual on-chain implementation is
// not yet wired (phase 1) — this struct + persistence exists so the UI can
// collect and store the values via setAnchorConfigJson before anchor methods
// become functional.
struct AnchorConfig {
    QString programId;        // 32-byte hex from `spel deploy`. No static default.
    QString sequencerUrl;     // Defaults to local sequencer http://127.0.0.1:3040.
    QString walletHome;       // Path to spel-framework wallet. Defaults to ../.scaffold/wallet.
    QString signerAccountId;  // Defaults to chronicle-registry dev signer.

    static AnchorConfig withDevDefaults();

    QStringList missingFields() const;  // empty when fully configured
    bool isConfigured() const { return missingFields().isEmpty(); }

    QJsonObject toJson() const;
    static AnchorConfig fromJson(const QJsonObject& obj);
};

// File-based persistence. Path is derived from AppDataLocation; load returns
// withDevDefaults() if the file is missing.
class AnchorConfigStore {
public:
    static QString configPath();
    static AnchorConfig load();
    static bool save(const AnchorConfig& cfg, QString* error = nullptr);
};

#endif // CHRONICLE_ANCHOR_CONFIG_H
