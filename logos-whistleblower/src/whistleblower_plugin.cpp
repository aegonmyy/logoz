#include "whistleblower_plugin.h"

#include <QDateTime>
#include <QDebug>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeDatabase>
#include <QMimeType>
#include <QStringList>
#include <QTimer>

#include "logos_api.h"
#include "logos_api_client.h"
#include "token_manager.h"

namespace {

// Delivery bring-up: retry up to 6 times with capped exponential backoff.
constexpr int kMaxDeliveryTries = 6;
constexpr int kDeliveryBaseMs   = 2500;
constexpr int kDeliveryCapMs    = 45000;

// Job status polling interval.
constexpr int kPollMs = 1200;

// Terminal job phases that stop polling.
constexpr auto kPhaseDone  = "done";
constexpr auto kPhaseError = "error";

QJsonObject parse(const QString& json) {
    return QJsonDocument::fromJson(json.toUtf8()).object();
}

QString compact(const QJsonObject& obj) {
    return QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

QString compactArr(const QJsonArray& arr) {
    return QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

QString formatErr(const QJsonObject& obj) {
    const QString code  = obj.value(QStringLiteral("code")).toString();
    const QString error = obj.value(QStringLiteral("error")).toString();
    return code.isEmpty() ? error : QStringLiteral("%1: %2").arg(code, error);
}

} // namespace

WhistleblowerPlugin::WhistleblowerPlugin(QObject* parent)
    : WhistleblowerSimpleSource(parent)
{
    setPhase(QStringLiteral("idle"));
    setWorking(false);
    setNetworkReady(false);
    setAnchorsMapJson(QStringLiteral("{}"));
}

WhistleblowerPlugin::~WhistleblowerPlugin() {
    if (m_pollTimer)    m_pollTimer->stop();
    if (m_deliveryRetry) m_deliveryRetry->stop();
    delete m_client;
}

void WhistleblowerPlugin::initLogos(LogosAPI* api) {
    m_api = api;
    setBackend(this);

    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(kPollMs);
    connect(m_pollTimer, &QTimer::timeout, this, &WhistleblowerPlugin::tickJobPoll);

    m_deliveryRetry = new QTimer(this);
    m_deliveryRetry->setSingleShot(true);
    connect(m_deliveryRetry, &QTimer::timeout, this, &WhistleblowerPlugin::connectDelivery);

    ensureClient();

    // Hydrate UI from persisted ledger without waiting for network.
    QTimer::singleShot(0, this, [this]() { reloadHistory(); });
    QTimer::singleShot(0, this, [this]() { reloadAnchorCaps(); });
    QTimer::singleShot(0, this, [this]() { reloadAnchors(); });

    // Kick off delivery bring-up; it self-retries via scheduleDeliveryRetry.
    QTimer::singleShot(0, this, [this]() { connectDelivery(); });
}

void WhistleblowerPlugin::ensureClient() {
    if (m_client || !m_api) return;
    m_client = new LogosAPIClient(
        QStringLiteral("chronicle"),
        QStringLiteral("whistleblower"),
        m_api->getTokenManager(),
        this);
}

QString WhistleblowerPlugin::invokeChronicle(const QString& method,
                                              const QVariantList& args)
{
    ensureClient();
    if (!m_client)
        return compact({
            {QStringLiteral("ok"),    false},
            {QStringLiteral("code"),  QStringLiteral("CLIENT_UNAVAILABLE")},
            {QStringLiteral("error"), QStringLiteral("chronicle client not ready")},
        });

    QVariant result;
    switch (args.size()) {
        case 0: result = m_client->invokeRemoteMethod(QStringLiteral("chronicle"), method); break;
        case 1: result = m_client->invokeRemoteMethod(QStringLiteral("chronicle"), method, args[0]); break;
        case 2: result = m_client->invokeRemoteMethod(QStringLiteral("chronicle"), method, args[0], args[1]); break;
        case 3: result = m_client->invokeRemoteMethod(QStringLiteral("chronicle"), method, args[0], args[1], args[2]); break;
        case 5: result = m_client->invokeRemoteMethod(QStringLiteral("chronicle"), method,
                    args[0], args[1], args[2], args[3], args[4]); break;
        default:
            qWarning() << "WhistleblowerPlugin: unsupported arg count" << args.size()
                       << "for method" << method;
            return {};
    }
    return result.toString();
}

// ── Delivery bring-up ────────────────────────────────────────────────────────

void WhistleblowerPlugin::connectDelivery() {
    const QJsonObject resp = parse(invokeChronicle(QStringLiteral("startBroadcasterJson")));
    const bool ok = resp.value(QStringLiteral("ok")).toBool();
    setNetworkReady(ok);

    if (ok) {
        m_deliveryTries = 0;
        reloadHistory();
        return;
    }

    ++m_deliveryTries;
    qWarning() << "WhistleblowerPlugin: connectDelivery attempt"
               << m_deliveryTries << "failed:" << formatErr(resp);
    if (m_deliveryTries < kMaxDeliveryTries) scheduleDeliveryRetry();
    else setLastErr(QStringLiteral("Delivery unavailable: ") + formatErr(resp));
}

void WhistleblowerPlugin::scheduleDeliveryRetry() {
    const int delay = std::min(kDeliveryBaseMs << (m_deliveryTries - 1), kDeliveryCapMs);
    m_deliveryRetry->start(delay);
}

// ── Submit (upload + broadcast) ──────────────────────────────────────────────

void WhistleblowerPlugin::submitDocument(QString path, QString contentType,
                                          QString title, QString description,
                                          QString tagsCsv)
{
    if (working()) {
        setLastErr(QStringLiteral("a publish is already in progress"));
        return;
    }

    // Auto-detect MIME type when not supplied.
    if (contentType.trimmed().isEmpty()) {
        QMimeDatabase db;
        const QMimeType mt = db.mimeTypeForFile(QFileInfo(path), QMimeDatabase::MatchDefault);
        contentType = mt.isValid() ? mt.name()
                                   : QStringLiteral("application/octet-stream");
    }

    QJsonArray tagsArr;
    for (const QString& raw : tagsCsv.split(u',', Qt::SkipEmptyParts)) {
        const QString t = raw.trimmed();
        if (!t.isEmpty()) tagsArr.append(t);
    }

    const QString reqJson = compact({
        {QStringLiteral("path"),         path},
        {QStringLiteral("content_type"), contentType},
        {QStringLiteral("title"),        title},
        {QStringLiteral("description"),  description},
        {QStringLiteral("tags"),         tagsArr},
        {QStringLiteral("broadcast"),    true},
    });

    clearJobState();
    setWorking(true);
    setPhase(QStringLiteral("queued"));

    handleJobResponse(invokeChronicle(QStringLiteral("publishFileJson"),
                                      QVariantList{reqJson}));
}

void WhistleblowerPlugin::handleJobResponse(const QString& resp) {
    const QJsonObject obj = parse(resp);
    if (!obj.value(QStringLiteral("queued")).toBool()) {
        setWorking(false);
        setPhase(QStringLiteral("error"));
        setLastErr(formatErr(obj));
        return;
    }
    setActiveJobId(obj.value(QStringLiteral("publish_id")).toString());
    if (m_pollTimer && !m_pollTimer->isActive()) m_pollTimer->start();
}

void WhistleblowerPlugin::tickJobPoll() {
    const QString jobId = activeJobId();
    if (jobId.isEmpty()) { m_pollTimer->stop(); return; }

    const QJsonObject obj = parse(
        invokeChronicle(QStringLiteral("publishStatusJson"), QVariantList{jobId}));

    const QString newPhase = obj.value(QStringLiteral("status")).toString();
    if (!newPhase.isEmpty()) setPhase(newPhase);

    const QString newCid = obj.value(QStringLiteral("cid")).toString();
    if (!newCid.isEmpty() && newCid != contentId()) setContentId(newCid);

    const QString newHash = obj.value(QStringLiteral("metadata_hash")).toString();
    if (!newHash.isEmpty() && newHash != docHash()) setDocHash(newHash);

    const bool terminal = (newPhase == QLatin1String(kPhaseDone)
                           || newPhase == QLatin1String(kPhaseError));
    if (terminal) {
        m_pollTimer->stop();
        setWorking(false);
        if (newPhase == QLatin1String(kPhaseError))
            setLastErr(formatErr(obj));
        reloadHistory();
    }
}

void WhistleblowerPlugin::clearJobState() {
    setActiveJobId(QString());
    setContentId(QString());
    setDocHash(QString());
    setLastErr(QString());
}

// ── History ──────────────────────────────────────────────────────────────────

void WhistleblowerPlugin::reloadHistory() {
    const QJsonObject obj = parse(invokeChronicle(QStringLiteral("listPublishedJson")));
    if (!obj.value(QStringLiteral("ok")).toBool()) return;
    const QJsonArray items = obj.value(QStringLiteral("items")).toArray();
    setHistoryJson(compactArr(items));
}

QString WhistleblowerPlugin::fetchHistoryJson() {
    return invokeChronicle(QStringLiteral("listPublishedJson"));
}

void WhistleblowerPlugin::discardHistory() {
    if (working()) {
        setLastErr(QStringLiteral("cannot clear while a publish is in progress"));
        return;
    }
    const QJsonObject obj = parse(invokeChronicle(QStringLiteral("clearPublishedJson")));
    if (!obj.value(QStringLiteral("ok")).toBool()) {
        setLastErr(formatErr(obj));
        return;
    }
    setPhase(QStringLiteral("idle"));
    setContentId(QString());
    setDocHash(QString());
    setActiveJobId(QString());
    setLastErr(QString());
    // Cleared publishes leave orphaned anchor entries with no UI row — wipe them.
    invokeChronicle(QStringLiteral("clearAnchorsJson"));
    reloadAnchors();
    reloadHistory();
}

// ── Anchor ───────────────────────────────────────────────────────────────────

void WhistleblowerPlugin::reloadAnchorCaps() {
    setAnchorCapsJson(invokeChronicle(QStringLiteral("anchorCapabilitiesJson")));
    setAnchorCfgJson(invokeChronicle(QStringLiteral("getAnchorConfigJson")));
}

void WhistleblowerPlugin::applyAnchorConfig(QString cfgJson) {
    const QJsonObject obj = parse(
        invokeChronicle(QStringLiteral("setAnchorConfigJson"), QVariantList{cfgJson}));
    if (!obj.value(QStringLiteral("ok")).toBool()) {
        setLastErr(formatErr(obj));
        return;
    }
    setLastErr(QString());
    reloadAnchorCaps();
}

void WhistleblowerPlugin::reloadAnchors() {
    const QJsonObject obj = parse(invokeChronicle(QStringLiteral("listAnchorsJson")));
    if (!obj.value(QStringLiteral("ok")).toBool()) return;
    const QJsonObject map = obj.value(QStringLiteral("anchors")).toObject();
    setAnchorsMapJson(QString::fromUtf8(
        QJsonDocument(map).toJson(QJsonDocument::Compact)));
}

void WhistleblowerPlugin::anchorJob(QString publishId) {
    // Look up CID + metadata_hash from the local history cache.
    const QJsonDocument histDoc = QJsonDocument::fromJson(historyJson().toUtf8());
    QJsonObject record;
    if (histDoc.isArray()) {
        for (const QJsonValue& v : histDoc.array()) {
            const QJsonObject candidate = v.toObject();
            if (candidate.value(QStringLiteral("publish_id")).toString() == publishId) {
                record = candidate;
                break;
            }
        }
    }
    if (record.isEmpty()) {
        setLastErr(QStringLiteral("publish record not found: %1").arg(publishId));
        return;
    }

    const QString reqJson = compact({
        {QStringLiteral("publish_id"),    publishId},
        {QStringLiteral("cid"),           record.value(QStringLiteral("cid")).toString()},
        {QStringLiteral("metadata_hash"), record.value(QStringLiteral("metadata_hash")).toString()},
        {QStringLiteral("timestamp"),     QDateTime::currentSecsSinceEpoch()},
    });
    const QJsonObject resp = parse(
        invokeChronicle(QStringLiteral("anchorBatchJson"), QVariantList{reqJson}));

    if (!resp.value(QStringLiteral("ok")).toBool())
        setLastErr(formatErr(resp));
    else
        setLastErr(QString());

    reloadAnchors();
}
