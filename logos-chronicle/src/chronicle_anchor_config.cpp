#include "chronicle_anchor_config.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QStandardPaths>

namespace {
constexpr const char* kDefaultSequencerUrl   = "http://127.0.0.1:3040";
constexpr const char* kDefaultWalletHome     = ".scaffold/wallet";
}

AnchorConfig AnchorConfig::withDevDefaults() {
    AnchorConfig c;
    // program_id and signer_account_id intentionally left empty — both are
    // wallet- and deployment-specific. The UI's AnchorConfigDialog prompts
    // the user for them on first anchor attempt (missing_fields surfaces them).
    c.sequencerUrl    = QString::fromLatin1(kDefaultSequencerUrl);
    c.walletHome      = QString::fromLatin1(kDefaultWalletHome);
    return c;
}

QStringList AnchorConfig::missingFields() const {
    QStringList m;
    if (programId.trimmed().isEmpty())        m.append(QStringLiteral("program_id"));
    if (sequencerUrl.trimmed().isEmpty())     m.append(QStringLiteral("sequencer_url"));
    if (walletHome.trimmed().isEmpty())       m.append(QStringLiteral("wallet_home"));
    if (signerAccountId.trimmed().isEmpty())  m.append(QStringLiteral("signer_account_id"));
    return m;
}

QJsonObject AnchorConfig::toJson() const {
    QJsonObject obj;
    obj.insert(QStringLiteral("program_id"),        programId);
    obj.insert(QStringLiteral("sequencer_url"),     sequencerUrl);
    obj.insert(QStringLiteral("wallet_home"),       walletHome);
    obj.insert(QStringLiteral("signer_account_id"), signerAccountId);
    return obj;
}

AnchorConfig AnchorConfig::fromJson(const QJsonObject& obj) {
    AnchorConfig c;
    c.programId        = obj.value(QStringLiteral("program_id")).toString();
    c.sequencerUrl     = obj.value(QStringLiteral("sequencer_url")).toString();
    c.walletHome       = obj.value(QStringLiteral("wallet_home")).toString();
    c.signerAccountId  = obj.value(QStringLiteral("signer_account_id")).toString();
    return c;
}

QString AnchorConfigStore::configPath() {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
           QStringLiteral("/chronicle/anchor-config.json");
}

AnchorConfig AnchorConfigStore::load() {
    QFile f(configPath());
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return AnchorConfig::withDevDefaults();
    }
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject()) {
        return AnchorConfig::withDevDefaults();
    }
    // Merge persisted values onto the dev defaults so fields the user hasn't
    // touched stay populated.
    AnchorConfig c = AnchorConfig::withDevDefaults();
    const QJsonObject obj = doc.object();
    if (obj.contains(QStringLiteral("program_id")))
        c.programId = obj.value(QStringLiteral("program_id")).toString();
    if (obj.contains(QStringLiteral("sequencer_url")))
        c.sequencerUrl = obj.value(QStringLiteral("sequencer_url")).toString();
    if (obj.contains(QStringLiteral("wallet_home")))
        c.walletHome = obj.value(QStringLiteral("wallet_home")).toString();
    if (obj.contains(QStringLiteral("signer_account_id")))
        c.signerAccountId = obj.value(QStringLiteral("signer_account_id")).toString();
    return c;
}

bool AnchorConfigStore::save(const AnchorConfig& cfg, QString* error) {
    const QString path = configPath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        if (error) *error = f.errorString();
        return false;
    }
    f.write(QJsonDocument(cfg.toJson()).toJson(QJsonDocument::Indented));
    return true;
}
