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
constexpr int POLL_INTERVAL_MS = 1000;
constexpr int MAX_BROADCASTER_RETRIES = 5;
constexpr int BROADCASTER_RETRY_BASE_MS = 2000;  // 2s, 4s, 8s, 16s, 30s (capped)
constexpr int BROADCASTER_RETRY_CAP_MS = 30000;

QString compactJson(const QJsonObject& obj) {
    return QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

QJsonObject parseObject(const QString& json) {
    return QJsonDocument::fromJson(json.toUtf8()).object();
}
}  // namespace

WhistleblowerPlugin::WhistleblowerPlugin(QObject* parent)
    : WhistleblowerSimpleSource(parent)
{
    setStatus(QStringLiteral("idle"));
    setBusy(false);
    setDeliveryReady(false);
}

WhistleblowerPlugin::~WhistleblowerPlugin() {
    if (m_pollTimer != nullptr) {
        m_pollTimer->stop();
    }
    delete m_chronicleClient;
}

void WhistleblowerPlugin::initLogos(LogosAPI* api) {
    m_logosAPI = api;
    setBackend(this);

    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(POLL_INTERVAL_MS);
    connect(m_pollTimer, &QTimer::timeout,
            this, &WhistleblowerPlugin::pollPublishStatus);

    m_startBroadcasterRetryTimer = new QTimer(this);
    m_startBroadcasterRetryTimer->setSingleShot(true);
    connect(m_startBroadcasterRetryTimer, &QTimer::timeout,
            this, &WhistleblowerPlugin::startBroadcaster);

    ensureChronicleClient();

    // Show local publish history immediately — chronicle's ledger read does
    // not depend on storage or delivery being initialised.
    QTimer::singleShot(0, this, [this]() { refreshPublishedList(); });

    setAnchorsJson(QStringLiteral("{}"));
    // Anchor capabilities + saved config + persisted anchor map — file-only
    // reads, no on-chain calls.
    QTimer::singleShot(0, this, [this]() { refreshAnchorCapabilities(); });
    QTimer::singleShot(0, this, [this]() { refreshAnchors(); });

    // Pre-warm Chronicle's delivery+storage services. First attempt usually
    // races with delivery_module's slow Waku-node bring-up; startBroadcaster
    // self-retries with backoff if it fails.
    QTimer::singleShot(0, this, [this]() { startBroadcaster(); });

    qDebug() << "WhistleblowerPlugin: initialized";
}

void WhistleblowerPlugin::ensureChronicleClient() {
    if (m_chronicleClient != nullptr || m_logosAPI == nullptr) {
        return;
    }
    m_chronicleClient = new LogosAPIClient(
        QStringLiteral("chronicle"),
        QStringLiteral("whistleblower"),
        m_logosAPI->getTokenManager(),
        this);
}

QString WhistleblowerPlugin::callChronicle(const QString& method,
                                           const QVariantList& args) {
    ensureChronicleClient();
    if (m_chronicleClient == nullptr) {
        return compactJson({{QStringLiteral("ok"), false},
                            {QStringLiteral("code"), QStringLiteral("CLIENT_UNAVAILABLE")},
                            {QStringLiteral("error"), QStringLiteral("chronicle client not ready")}});
    }

    QVariant response;
    if (args.isEmpty()) {
        response = m_chronicleClient->invokeRemoteMethod(
            QStringLiteral("chronicle"), method);
    } else if (args.size() == 1) {
        response = m_chronicleClient->invokeRemoteMethod(
            QStringLiteral("chronicle"), method, args[0]);
    } else if (args.size() == 2) {
        response = m_chronicleClient->invokeRemoteMethod(
            QStringLiteral("chronicle"), method, args[0], args[1]);
    } else if (args.size() == 3) {
        response = m_chronicleClient->invokeRemoteMethod(
            QStringLiteral("chronicle"), method, args[0], args[1], args[2]);
    } else if (args.size() == 5) {
        response = m_chronicleClient->invokeRemoteMethod(
            QStringLiteral("chronicle"), method,
            args[0], args[1], args[2], args[3], args[4]);
    } else {
        qWarning() << "WhistleblowerPlugin: unsupported arg count for"
                   << method << args.size();
        return {};
    }
    return response.toString();
}

void WhistleblowerPlugin::startBroadcaster() {
    const QString resp = callChronicle(QStringLiteral("startBroadcasterJson"));
    const QJsonObject obj = parseObject(resp);
    const bool ok = obj.value(QStringLiteral("ok")).toBool();
    setDeliveryReady(ok);

    if (ok) {
        m_startBroadcasterAttempts = 0;
        qDebug() << "WhistleblowerPlugin: chronicle broadcaster ready";
        // Refresh history once services come up — in case the ledger was
        // updated externally while we were waiting.
        refreshPublishedList();
        return;
    }

    const QString err = obj.value(QStringLiteral("error")).toString();
    m_startBroadcasterAttempts++;
    qWarning() << "WhistleblowerPlugin: startBroadcaster attempt"
               << m_startBroadcasterAttempts << "failed:" << err;

    if (m_startBroadcasterAttempts < MAX_BROADCASTER_RETRIES) {
        const int delay = std::min(
            BROADCASTER_RETRY_BASE_MS * (1 << (m_startBroadcasterAttempts - 1)),
            BROADCASTER_RETRY_CAP_MS);
        qDebug() << "WhistleblowerPlugin: retrying startBroadcaster in"
                 << delay << "ms";
        m_startBroadcasterRetryTimer->start(delay);
    } else {
        qWarning() << "WhistleblowerPlugin: giving up on startBroadcaster after"
                   << m_startBroadcasterAttempts << "attempts";
        setLastError(QStringLiteral("Could not initialise chronicle services: %1")
                         .arg(err));
    }
}

void WhistleblowerPlugin::refreshPublishedList() {
    const QString resp = callChronicle(QStringLiteral("listPublishedJson"));
    const QJsonObject obj = parseObject(resp);
    if (!obj.value(QStringLiteral("ok")).toBool()) {
        return;
    }
    const QJsonArray records = obj.value(QStringLiteral("records")).toArray();
    setPublishedRecordsJson(QString::fromUtf8(
        QJsonDocument(records).toJson(QJsonDocument::Compact)));
}

void WhistleblowerPlugin::refreshAnchorCapabilities() {
    setAnchorCapabilitiesJson(callChronicle(QStringLiteral("anchorCapabilitiesJson")));
    setAnchorConfigJson(callChronicle(QStringLiteral("getAnchorConfigJson")));
}

void WhistleblowerPlugin::setAnchorConfig(QString cfgJson) {
    const QString resp = callChronicle(
        QStringLiteral("setAnchorConfigJson"),
        QVariantList{cfgJson});
    const QJsonObject obj = parseObject(resp);
    if (!obj.value(QStringLiteral("ok")).toBool()) {
        const QString code  = obj.value(QStringLiteral("code")).toString();
        const QString error = obj.value(QStringLiteral("error")).toString();
        setLastError(code.isEmpty() ? error
                                    : QStringLiteral("%1: %2").arg(code, error));
        return;
    }
    setLastError(QString());
    refreshAnchorCapabilities();
}

void WhistleblowerPlugin::refreshAnchors() {
    // Chronicle is authoritative — read its persisted map and mirror to the
    // QML-facing property. Keyed by CID (matches the on-chain identifier).
    const QString resp = callChronicle(QStringLiteral("listAnchorsJson"));
    const QJsonObject obj = parseObject(resp);
    if (!obj.value(QStringLiteral("ok")).toBool()) return;
    const QJsonObject anchors = obj.value(QStringLiteral("anchors")).toObject();
    setAnchorsJson(QString::fromUtf8(
        QJsonDocument(anchors).toJson(QJsonDocument::Compact)));
}

void WhistleblowerPlugin::anchorPublished(QString publishId) {
    // Look up the publish record from the cached list so we can build the
    // (cid, metadata_hash, timestamp) tuple. Parsing the JSON each click is
    // fine for phase 1 — list size is small.
    const QJsonDocument recordsDoc =
        QJsonDocument::fromJson(publishedRecordsJson().toUtf8());
    QJsonObject record;
    if (recordsDoc.isArray()) {
        for (const QJsonValue& v : recordsDoc.array()) {
            const QJsonObject candidate = v.toObject();
            if (candidate.value(QStringLiteral("publish_id")).toString() == publishId) {
                record = candidate;
                break;
            }
        }
    }
    if (record.isEmpty()) {
        setLastError(QStringLiteral("publish record not found: %1").arg(publishId));
        return;
    }

    const QString cid   = record.value(QStringLiteral("cid")).toString();
    const QString mhash = record.value(QStringLiteral("metadata_hash")).toString();
    // Per LP-17, `anchor_timestamp` is "when this was anchored", not when the
    // document was published. Stamp it at click time, in seconds.
    const qint64 tsSec = QDateTime::currentSecsSinceEpoch();

    QJsonObject entry;
    entry.insert(QStringLiteral("publish_id"), publishId);
    entry.insert(QStringLiteral("cid"), cid);
    entry.insert(QStringLiteral("metadata_hash"), mhash);
    entry.insert(QStringLiteral("timestamp"), tsSec);

    QJsonArray entries;
    entries.append(entry);
    QJsonObject req;
    req.insert(QStringLiteral("entries"), entries);
    const QString requestJson = QString::fromUtf8(
        QJsonDocument(req).toJson(QJsonDocument::Compact));

    const QString resp = callChronicle(
        QStringLiteral("anchorBatchJson"),
        QVariantList{requestJson});
    const QJsonObject obj = parseObject(resp);

    if (!obj.value(QStringLiteral("ok")).toBool()) {
        const QString code  = obj.value(QStringLiteral("code")).toString();
        const QString error = obj.value(QStringLiteral("error")).toString();
        setLastError(code.isEmpty() ? error
                                    : QStringLiteral("%1: %2").arg(code, error));
    } else {
        setLastError(QString());
    }

    // Chronicle has persisted the resulting record (or skipped persistence
    // because the attempt never reached the chain). Re-read its authoritative
    // map rather than mirroring locally.
    refreshAnchors();
}

void WhistleblowerPlugin::clearHistory() {
    if (busy()) {
        setLastError(QStringLiteral("cannot clear while a publish is in progress"));
        return;
    }
    const QString resp = callChronicle(QStringLiteral("clearPublishedJson"));
    const QJsonObject obj = parseObject(resp);
    if (!obj.value(QStringLiteral("ok")).toBool()) {
        const QString code  = obj.value(QStringLiteral("code")).toString();
        const QString error = obj.value(QStringLiteral("error")).toString();
        setLastError(code.isEmpty() ? error
                                    : QStringLiteral("%1: %2").arg(code, error));
        return;
    }
    // Clear the status panel too — last-publish details would be misleading
    // once the underlying records are gone.
    setStatus(QStringLiteral("idle"));
    setCid(QString());
    setMetadataHash(QString());
    setCurrentPublishId(QString());
    setLastError(QString());
    // Anchor records are keyed by CID — cleared publishes mean orphaned
    // anchor entries with no UI row to render on. Wipe them too.
    callChronicle(QStringLiteral("clearAnchorsJson"));
    refreshAnchors();
    refreshPublishedList();
}

void WhistleblowerPlugin::resetPublishState() {
    setCurrentPublishId(QString());
    setCid(QString());
    setMetadataHash(QString());
    setLastError(QString());
}

void WhistleblowerPlugin::publish(QString path,
                                  QString contentType,
                                  QString title,
                                  QString description,
                                  QString tagsCsv) {
    if (busy()) {
        setLastError(QStringLiteral("a publish is already in progress"));
        return;
    }

    QJsonArray tagsArr;
    for (const QString& raw : tagsCsv.split(',', Qt::SkipEmptyParts)) {
        const QString tag = raw.trimmed();
        if (!tag.isEmpty()) {
            tagsArr.append(tag);
        }
    }

    QString resolvedContentType = contentType.trimmed();
    if (resolvedContentType.isEmpty()) {
        // Detect via Qt's MIME database — checks extension AND sniffs the file
        // header, falls back to application/octet-stream.
        QMimeDatabase db;
        const QMimeType mt = db.mimeTypeForFile(
            QFileInfo(path), QMimeDatabase::MatchDefault);
        resolvedContentType = mt.isValid()
            ? mt.name()
            : QStringLiteral("application/octet-stream");
    }

    QJsonObject req;
    req.insert(QStringLiteral("path"), path);
    req.insert(QStringLiteral("content_type"), resolvedContentType);
    req.insert(QStringLiteral("title"), title);
    req.insert(QStringLiteral("description"), description);
    req.insert(QStringLiteral("tags"), tagsArr);
    req.insert(QStringLiteral("broadcast"), true);

    resetPublishState();
    setBusy(true);
    setStatus(QStringLiteral("queued"));

    const QString resp = callChronicle(
        QStringLiteral("publishFileJson"),
        QVariantList{compactJson(req)});
    handlePublishResponse(resp);
}

void WhistleblowerPlugin::handlePublishResponse(const QString& responseJson) {
    const QJsonObject obj = parseObject(responseJson);

    if (!obj.value(QStringLiteral("queued")).toBool()) {
        setBusy(false);
        setStatus(QStringLiteral("error"));
        const QString code  = obj.value(QStringLiteral("code")).toString();
        const QString error = obj.value(QStringLiteral("error")).toString();
        setLastError(code.isEmpty() ? error
                                    : QStringLiteral("%1: %2").arg(code, error));
        return;
    }

    setCurrentPublishId(obj.value(QStringLiteral("publish_id")).toString());
    if (m_pollTimer != nullptr && !m_pollTimer->isActive()) {
        m_pollTimer->start();
    }
}

void WhistleblowerPlugin::pollPublishStatus() {
    const QString publishId = currentPublishId();
    if (publishId.isEmpty()) {
        m_pollTimer->stop();
        return;
    }

    const QString resp = callChronicle(
        QStringLiteral("publishStatusJson"),
        QVariantList{publishId});
    const QJsonObject obj = parseObject(resp);

    const QString newStatus = obj.value(QStringLiteral("status")).toString();
    if (!newStatus.isEmpty()) {
        setStatus(newStatus);
    }

    const QString newCid = obj.value(QStringLiteral("cid")).toString();
    if (!newCid.isEmpty() && newCid != cid()) {
        setCid(newCid);
    }
    const QString newHash = obj.value(QStringLiteral("metadata_hash")).toString();
    if (!newHash.isEmpty() && newHash != metadataHash()) {
        setMetadataHash(newHash);
    }

    const bool terminal = (newStatus == QStringLiteral("broadcast_sent") ||
                           newStatus == QStringLiteral("error"));
    if (terminal) {
        m_pollTimer->stop();
        setBusy(false);
        if (newStatus == QStringLiteral("error")) {
            const QString code  = obj.value(QStringLiteral("code")).toString();
            const QString error = obj.value(QStringLiteral("error")).toString();
            setLastError(code.isEmpty() ? error
                                        : QStringLiteral("%1: %2").arg(code, error));
        }
        // History changed (either a new broadcast_sent record landed, or an
        // error record was persisted). Push the latest list to the UI.
        refreshPublishedList();
    }
}

QString WhistleblowerPlugin::listPublishedJson() {
    return callChronicle(QStringLiteral("listPublishedJson"));
}
