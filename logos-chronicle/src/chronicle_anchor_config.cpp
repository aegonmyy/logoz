#include "chronicle_anchor_config.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QStandardPaths>

namespace {
const char* kDefaultSequencer = "http://127.0.0.1:3040";
const char* kDefaultWallet    = ".scaffold/wallet";
} // namespace

AnchorConfig AnchorConfig::devDefaults() {
    AnchorConfig c;
    c.sequencerUrl = QString::fromLatin1(kDefaultSequencer);
    c.walletHome   = QString::fromLatin1(kDefaultWallet);
    return c;
}

QStringList AnchorConfig::missingFields() const {
    QStringList out;
    if (programId.trimmed().isEmpty())       out << QStringLiteral("program_id");
    if (sequencerUrl.trimmed().isEmpty())    out << QStringLiteral("sequencer_url");
    if (walletHome.trimmed().isEmpty())      out << QStringLiteral("wallet_home");
    if (signerAccountId.trimmed().isEmpty()) out << QStringLiteral("signer_account_id");
    return out;
}

QJsonObject AnchorConfig::toJson() const {
    return {
        {QStringLiteral("program_id"),        programId},
        {QStringLiteral("sequencer_url"),     sequencerUrl},
        {QStringLiteral("wallet_home"),       walletHome},
        {QStringLiteral("signer_account_id"), signerAccountId},
    };
}

AnchorConfig AnchorConfig::fromJson(const QJsonObject& obj) {
    AnchorConfig c = devDefaults();
    auto get = [&](const char* k) -> QString {
        return obj.value(QLatin1String(k)).toString();
    };
    if (obj.contains(QStringLiteral("program_id")))
        c.programId = get("program_id");
    if (obj.contains(QStringLiteral("sequencer_url")))
        c.sequencerUrl = get("sequencer_url");
    if (obj.contains(QStringLiteral("wallet_home")))
        c.walletHome = get("wallet_home");
    if (obj.contains(QStringLiteral("signer_account_id")))
        c.signerAccountId = get("signer_account_id");
    return c;
}

QString AnchorStore::path() {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
           + QStringLiteral("/chronicle/anchor.json");
}

AnchorConfig AnchorStore::load() {
    QFile f(path());
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return AnchorConfig::devDefaults();
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    return doc.isObject() ? AnchorConfig::fromJson(doc.object())
                          : AnchorConfig::devDefaults();
}

bool AnchorStore::save(const AnchorConfig& cfg, QString* err) {
    const QString p = path();
    QDir().mkpath(QFileInfo(p).absolutePath());
    QFile f(p);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        if (err) *err = f.errorString();
        return false;
    }
    f.write(QJsonDocument(cfg.toJson()).toJson(QJsonDocument::Indented));
    return true;
}
