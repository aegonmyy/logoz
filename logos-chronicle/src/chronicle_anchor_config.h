#ifndef CHRONICLE_ANCHOR_CONFIG_H
#define CHRONICLE_ANCHOR_CONFIG_H

#include <QJsonObject>
#include <QString>
#include <QStringList>

// On-chain anchor settings. Persisted as JSON under AppDataLocation.
// programId and signerAccountId have no defaults — the user supplies them
// via setAnchorConfigJson(); missing fields surface through missingFields().
struct AnchorConfig {
    QString programId;
    QString sequencerUrl;
    QString walletHome;
    QString signerAccountId;

    static AnchorConfig devDefaults();

    QStringList missingFields() const;
    bool        isReady() const { return missingFields().isEmpty(); }

    QJsonObject         toJson()            const;
    static AnchorConfig fromJson(const QJsonObject& obj);
};

// JSON file persistence under AppDataLocation/chronicle/
class AnchorStore {
public:
    static QString      path();
    static AnchorConfig load();
    static bool         save(const AnchorConfig& cfg, QString* err = nullptr);
};

#endif // CHRONICLE_ANCHOR_CONFIG_H
