#include "chronicle_plugin.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaType>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QUuid>

#include "chronicle_helpers.h"
#include "logos_api_client.h"
#include "logos_object.h"
#include "token_manager.h"

using namespace chronicle;

namespace {

constexpr int    kUploadDelayMs    = 100;
constexpr int    kChunkSize        = 64 * 1024;
constexpr char   kDefaultTopic[]   = "/chronicle/1/document-index/json";
constexpr char   kDeliveryConfig[] =
    R"({"logLevel":"INFO","mode":"Core","preset":"logos.dev"})";

// ── JSON helpers ─────────────────────────────────────────────────────────

QString compact(const QJsonObject& obj) {
    return QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

QString okJson(QJsonObject obj) {
    obj.insert(QStringLiteral("ok"), true);
    return compact(obj);
}

QString failJson(const QString& code, const QString& msg) {
    return compact({
        {QStringLiteral("ok"),    false},
        {QStringLiteral("code"),  code},
        {QStringLiteral("error"), msg},
    });
}

// ── Input parsing ─────────────────────────────────────────────────────────

bool parseTags(const QString& json, QStringList* out, QString* err) {
    out->clear();
    const QString s = json.trimmed();
    if (s.isEmpty()) return true;
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(s.toUtf8(), &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isArray()) {
        if (err) *err = QStringLiteral("tags must be a JSON array of strings");
        return false;
    }
    for (const QJsonValue& v : doc.array()) {
        if (!v.isString()) {
            if (err) *err = QStringLiteral("tags array must contain only strings");
            return false;
        }
        out->append(v.toString());
    }
    return true;
}

bool readInt(const QJsonObject& obj, const char* key, qint64* out, QString* err) {
    const QJsonValue v = obj.value(QLatin1String(key));
    if (v.isDouble()) {
        const double d = v.toDouble(-1);
        const qint64 i = static_cast<qint64>(d);
        if (d >= 0 && static_cast<double>(i) == d) { if (out) *out = i; return true; }
    }
    if (v.isString()) {
        bool ok = false;
        const qint64 i = v.toString().trimmed().toLongLong(&ok);
        if (ok && i >= 0) { if (out) *out = i; return true; }
    }
    if (err) *err = QStringLiteral("%1 must be a non-negative integer")
                        .arg(QString::fromLatin1(key));
    return false;
}

// ── Module-call outcome ───────────────────────────────────────────────────

struct CallResult { bool ok = false; QString value; QString error; };

CallResult parseCall(const QVariant& v) {
    CallResult r;
    if (!v.isValid()) { r.error = QStringLiteral("no response"); return r; }
    if (v.typeId() == QMetaType::Bool) {
        r.ok = v.toBool();
        if (!r.ok) r.error = QStringLiteral("call returned false");
        return r;
    }
    const QString text = v.toString();
    if (text.isEmpty()) { r.error = QStringLiteral("empty response"); return r; }
    const QJsonObject obj = QJsonDocument::fromJson(text.toUtf8()).object();
    if (obj.isEmpty()) { r.ok = true; r.value = text; return r; }
    r.ok    = obj.value(QStringLiteral("success")).toBool(
                  obj.value(QStringLiteral("ok")).toBool());
    r.error = obj.value(QStringLiteral("error")).toString();
    r.value = r.ok ? obj.value(QStringLiteral("value")).toString(text) : QString();
    return r;
}

} // anonymous namespace

// ── Construction / destruction ────────────────────────────────────────────

ChroniclePlugin::ChroniclePlugin() : QObject() {
    qRegisterMetaType<LogosResult>("LogosResult");
    m_broadcastTopic = QString::fromLatin1(kDefaultTopic);
}

ChroniclePlugin::~ChroniclePlugin() {
    delete m_storageClient;
    delete m_deliveryClient;
    delete m_ffi;
}

// ── Initialisation ─────────────────────────────────────────────────────────

void ChroniclePlugin::initLogos(LogosAPI* api) {
    m_api = api;
    logosAPI = api;

    auto seedToken = [&](const char* mod, const char* tok) {
        if (auto* tm = m_api->getTokenManager();
            tm && tm->getToken(QString::fromLatin1(mod)).isEmpty())
            tm->saveToken(QString::fromLatin1(mod), QString::fromLatin1(tok));
    };
    seedToken("storage_module",  "chronicle_headless_storage_token_v1");
    seedToken("delivery_module", "chronicle_headless_delivery_token_v1");

    initStorage();
    initDelivery();
    loadPublishLedger();

    m_anchorCfg       = AnchorStore::load();
    m_anchorCfgLoaded = true;
    loadAnchorLedger();

    QTimer::singleShot(0, this, [this]() {
        // Subscribe to storage events on the next event-loop iteration so
        // the API client is fully wired before we try to request an object.
        if (!m_storageSubscribed && m_storageClient) {
            m_storageEvents = m_storageClient->requestObject(
                QStringLiteral("storage_module"), Timeout(10000));
            if (m_storageEvents) {
                m_storageClient->onEvent(
                    m_storageEvents,
                    QStringLiteral("storageUploadDone"),
                    [this](const QString&, const QVariantList& args) {
                        onUploadDone(args);
                    });
                m_storageSubscribed = true;
            }
        }
    });
}

void ChroniclePlugin::initStorage() {
    if (m_storageClient || !m_api) return;
    m_storageClient = new LogosAPIClient(
        QStringLiteral("storage_module"),
        QStringLiteral("chronicle"),
        m_api->getTokenManager(), this);
}

void ChroniclePlugin::initDelivery() {
    if (m_deliveryClient || !m_api) return;
    m_deliveryClient = new LogosAPIClient(
        QStringLiteral("delivery_module"),
        QStringLiteral("chronicle"),
        m_api->getTokenManager(), this);
}

bool ChroniclePlugin::storageReady(QString* err) {
    initStorage();
    if (!m_storageClient) {
        if (err) *err = QStringLiteral("storage_module client unavailable");
        return false;
    }

    auto isAlready = [](const QString& e) {
        return e.toLower().contains(QStringLiteral("already"));
    };

    if (!m_storageReady) {
        const QString dataDir =
            QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
            + QStringLiteral("/chronicle/storage");
        QDir().mkpath(dataDir);

        const CallResult init = parseCall(m_storageClient->invokeRemoteMethod(
            QStringLiteral("storage_module"), QStringLiteral("init"),
            compact({{"data-dir", dataDir}})));
        if (!init.ok && !isAlready(init.error)) {
            if (err) *err = QStringLiteral("storage init: ") + init.error;
            return false;
        }

        const CallResult start = parseCall(m_storageClient->invokeRemoteMethod(
            QStringLiteral("storage_module"), QStringLiteral("start")));
        if (!start.ok && !isAlready(start.error)) {
            if (err) *err = QStringLiteral("storage start: ") + start.error;
            return false;
        }
        m_storageReady = true;
    }
    return true;
}

bool ChroniclePlugin::deliveryReady(QString* err) {
    initDelivery();
    if (!m_deliveryClient) {
        if (err) *err = QStringLiteral("delivery_module client unavailable");
        return false;
    }
    if (!m_deliveryObjReady) {
        m_deliveryObj = m_deliveryClient->requestObject(
            QStringLiteral("delivery_module"), Timeout(10000));
        if (!m_deliveryObj) {
            if (err) *err = QStringLiteral("could not acquire delivery object");
            return false;
        }
        m_deliveryObjReady = true;
    }
    if (!m_deliveryNodeUp) {
        const CallResult cr = parseCall(m_deliveryClient->invokeRemoteMethod(
            QStringLiteral("delivery_module"), QStringLiteral("createNode"),
            QString::fromLatin1(kDeliveryConfig)));
        if (!cr.ok) {
            if (err) *err = QStringLiteral("createNode: ") + cr.error;
            return false;
        }
        m_deliveryNodeUp = true;
    }
    if (!m_deliveryStarted) {
        const CallResult cr = parseCall(m_deliveryClient->invokeRemoteMethod(
            QStringLiteral("delivery_module"), QStringLiteral("start")));
        if (!cr.ok) {
            if (err) *err = QStringLiteral("start: ") + cr.error;
            return false;
        }
        m_deliveryStarted = true;
    }
    return true;
}

// ── Public API ────────────────────────────────────────────────────────────

QString ChroniclePlugin::health() { return QStringLiteral("ok"); }

QString ChroniclePlugin::normalizeContentTypeJson(const QString& ct) {
    return okJson({{QStringLiteral("content_type"), coerceContentType(ct)}});
}

QString ChroniclePlugin::hashMetadataJson(const QString& contentType,
                                          const QString& sizeBytes,
                                          const QString& title,
                                          const QString& description,
                                          const QString& tagsJson)
{
    QStringList tags;
    QString pe;
    if (!parseTags(tagsJson, &tags, &pe))
        return failJson(QStringLiteral("INVALID_TAGS"), pe);
    bool ok = false;
    const qint64 sz = sizeBytes.trimmed().toLongLong(&ok);
    if (!ok || sz < 0)
        return failJson(QStringLiteral("INVALID_NUMBER"),
                        QStringLiteral("sizeBytes must be a non-negative integer"));
    const QString hash = metadataHash(contentType, sz, title, description, tags);
    const QString canon = QString::fromUtf8(
        canonicalMetadata(contentType, sz, title, description, tags));
    return okJson({{QStringLiteral("metadata_hash"),  hash},
                   {QStringLiteral("canonical_json"), canon}});
}

QString ChroniclePlugin::buildMetadataEnvelopeJson(const QString& inputJson) {
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(inputJson.toUtf8(), &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject())
        return failJson(QStringLiteral("INVALID_INPUT"),
                        QStringLiteral("input must be a JSON object"));

    const QJsonObject in = doc.object();
    const QString cid   = in.value(QStringLiteral("cid")).toString();
    const QString ct    = in.value(QStringLiteral("content_type")).toString();
    const QString title = in.value(QStringLiteral("title")).toString();
    const QString desc  = in.value(QStringLiteral("description")).toString();

    if (cid.trimmed().isEmpty())
        return failJson(QStringLiteral("EMPTY_CID"), QStringLiteral("cid is required"));
    if (cleanTitle(title).isEmpty())
        return failJson(QStringLiteral("EMPTY_TITLE"),
                        QStringLiteral("title must contain visible characters"));

    QString err;
    qint64 sz = 0, ts = 0;
    QStringList tags;
    if (!readInt(in, "size_bytes", &sz, &err))
        return failJson(QStringLiteral("INVALID_NUMBER"), err);
    if (!readInt(in, "timestamp",  &ts, &err))
        return failJson(QStringLiteral("INVALID_NUMBER"), err);
    if (!parseTags(in.value(QStringLiteral("tags")).toVariant().toString(),
                   &tags, &err))
        return failJson(QStringLiteral("INVALID_TAGS"), err);

    const QString hash = metadataHash(ct, sz, title, desc, tags);
    const QJsonObject env = buildEnvelope(cid, ct, sz, ts, title, desc, tags, hash);
    if (!envelopeFitsInCap(env))
        return failJson(QStringLiteral("ENVELOPE_TOO_LARGE"),
                        QStringLiteral("envelope exceeds %1 byte cap")
                            .arg(kMaxEnvelopeBytes));

    return okJson({{QStringLiteral("metadata_hash"), hash},
                   {QStringLiteral("envelope"),      env}});
}

// ── Upload ────────────────────────────────────────────────────────────────

QString ChroniclePlugin::uploadFileJson(const QString& path,
                                        const QString& contentType,
                                        const QString& title)
{
    QFileInfo fi(path);
    if (!fi.exists() || !fi.isFile())
        return failJson(QStringLiteral("FILE_READ_FAILED"),
                        QStringLiteral("file not found: ") + path);
    if (fi.size() > kMaxFileBytes)
        return failJson(QStringLiteral("OVERSIZED"),
                        QStringLiteral("file exceeds 100 MB cap"));
    if (cleanTitle(title).isEmpty())
        return failJson(QStringLiteral("EMPTY_TITLE"),
                        QStringLiteral("title must contain visible characters"));

    const QString uid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString ct  = coerceContentType(contentType);
    const QString ext = fileExtension(ct);
    const QString base = cleanTitle(title);
    const QString fname = ext.isEmpty() ? base : base + QStringLiteral(".") + ext;
    const QString stageDir = QDir::temp().filePath(
        QStringLiteral("chronicle/%1").arg(uid));
    QDir().mkpath(stageDir);
    const QString staged = QDir(stageDir).filePath(fname);
    QFile::remove(staged);
    if (!QFile::copy(fi.absoluteFilePath(), staged)) {
        QDir(stageDir).removeRecursively();
        return failJson(QStringLiteral("FILE_READ_FAILED"),
                        QStringLiteral("could not stage file to: ") + staged);
    }

    UploadJob job;
    job.id         = uid;
    job.status     = QStringLiteral("queued");
    job.path       = path;
    job.stagedPath = staged;
    job.stagingDir = stageDir;
    job.contentType = ct;
    job.title      = cleanTitle(title);
    job.sizeBytes  = fi.size();
    job.startMs    = QDateTime::currentMSecsSinceEpoch();
    job.deadlineMs = job.startMs + uploadBudgetMs(fi.size());
    m_uploads.insert(uid, job);

    if (m_activeUploadId.isEmpty())
        QTimer::singleShot(kUploadDelayMs, this, [this, uid]() { startUpload(uid); });

    return okJson({{QStringLiteral("queued"),    true},
                   {QStringLiteral("upload_id"), uid}});
}

void ChroniclePlugin::startUpload(const QString& uid) {
    auto it = m_uploads.find(uid);
    if (it == m_uploads.end()) return;
    if (!m_activeUploadId.isEmpty() && m_activeUploadId != uid) return;
    if (it->status == QStringLiteral("uploading")) return;

    m_activeUploadId = uid;
    it->status = QStringLiteral("uploading");
    it->attempt++;
    const int attempt = it->attempt;
    const qint64 timeoutMs = uploadTimeoutMs(it->sizeBytes);

    if (!m_storageSubscribed) {
        failUpload(uid, QStringLiteral("STORAGE_UNAVAILABLE"),
                   QStringLiteral("storage events not subscribed"), true);
        return;
    }

    QTimer::singleShot(static_cast<int>(timeoutMs), this, [this, uid, attempt]() {
        auto it2 = m_uploads.find(uid);
        if (it2 == m_uploads.end() || it2->attempt != attempt
            || it2->status != QStringLiteral("uploading")) return;
        failUpload(uid, QStringLiteral("STORAGE_UNAVAILABLE"),
                   QStringLiteral("upload attempt timed out"), true);
    });

    const LogosResult res = parseLogosResult(
        m_storageClient->invokeRemoteMethod(
            QStringLiteral("storage_module"), QStringLiteral("uploadUrl"),
            QVariant::fromValue(QUrl::fromLocalFile(it->stagedPath)),
            kChunkSize, Timeout(static_cast<int>(timeoutMs))));
    if (!res.success) {
        const QString e = res.getError();
        failUpload(uid, isTransient(e) ? QStringLiteral("STORAGE_UNAVAILABLE")
                                       : QStringLiteral("STORAGE_REJECTED"),
                   e, isTransient(e));
        return;
    }

    const QString sessionId = res.value.toString();
    if (sessionId.isEmpty()) {
        failUpload(uid, QStringLiteral("STORAGE_REJECTED"),
                   QStringLiteral("uploadUrl returned empty session id"), false);
        return;
    }
    it = m_uploads.find(uid);
    if (it != m_uploads.end()) {
        it->sessionId = sessionId;
        m_sessionToUpload.insert(sessionId, uid);
    }
}

void ChroniclePlugin::onUploadDone(const QVariantList& args) {
    bool ok = true;
    QString sessionId, cid;

    if (args.size() >= 3 && args[0].typeId() == QMetaType::Bool) {
        ok = args[0].toBool(); sessionId = args[1].toString(); cid = args[2].toString();
    } else if (args.size() >= 2 && args[0].typeId() == QMetaType::Bool) {
        ok = args[0].toBool(); cid = args[1].toString();
    } else if (args.size() >= 2) {
        sessionId = args[0].toString(); cid = args[1].toString();
    } else if (args.size() == 1) {
        if (args[0].typeId() == QMetaType::Bool) ok = args[0].toBool();
        else cid = args[0].toString();
    }

    QString uid;
    if (!sessionId.isEmpty()) uid = m_sessionToUpload.take(sessionId);
    if (uid.isEmpty()) {
        for (auto it = m_uploads.begin(); it != m_uploads.end(); ++it) {
            if (it->status == QStringLiteral("uploading")) { uid = it.key(); break; }
        }
    }
    if (uid.isEmpty()) { qWarning() << "onUploadDone: no pending upload" << args; return; }

    if (!ok || cid.isEmpty()) {
        const QString e = cid.isEmpty()
            ? QStringLiteral("storageUploadDone missing CID") : cid;
        failUpload(uid, isTransient(e) ? QStringLiteral("STORAGE_UNAVAILABLE")
                                       : QStringLiteral("STORAGE_REJECTED"),
                   e, isTransient(e));
        return;
    }

    auto it = m_uploads.find(uid);
    if (it == m_uploads.end()) return;

    if (!it->sessionId.isEmpty()) m_sessionToUpload.remove(it->sessionId);
    it->status    = QStringLiteral("uploaded");
    it->cid       = cid;
    it->metaHash  = metadataHash(it->contentType, it->sizeBytes,
                                 it->title, it->description, it->tags);
    it->envelope  = buildEnvelope(cid, it->contentType, it->sizeBytes,
                                  QDateTime::currentSecsSinceEpoch(),
                                  it->title, it->description, it->tags, it->metaHash);
    if (!envelopeFitsInCap(it->envelope)) {
        failUpload(uid, QStringLiteral("ENVELOPE_TOO_LARGE"),
                   QStringLiteral("metadata envelope exceeds size cap"), false);
        return;
    }
    it->errCode.clear(); it->errMsg.clear();
    QDir(it->stagingDir).removeRecursively();

    const QString pubId = it->publishId;
    m_activeUploadId.clear();

    // Start next queued upload
    for (auto jt = m_uploads.begin(); jt != m_uploads.end(); ++jt) {
        if (jt->status == QStringLiteral("queued")) {
            const QString next = jt.key();
            QTimer::singleShot(0, this, [this, next]() { startUpload(next); });
            break;
        }
    }

    if (!pubId.isEmpty()) afterUpload(pubId);
}

void ChroniclePlugin::failUpload(const QString& uid, const QString& code,
                                 const QString& msg, bool retryable)
{
    auto it = m_uploads.find(uid);
    if (it == m_uploads.end()) return;
    if (!it->sessionId.isEmpty()) m_sessionToUpload.remove(it->sessionId);

    if (retryable) {
        const auto delay = backoffFor(it->attempt);
        const qint64 delayMs = static_cast<qint64>(delay.count());
        if (QDateTime::currentMSecsSinceEpoch() + delayMs > it->deadlineMs) {
            it->status  = QStringLiteral("error");
            it->errCode = QStringLiteral("RETRIES_EXHAUSTED");
            it->errMsg  = msg;
            QDir(it->stagingDir).removeRecursively();
            const QString pubId = it->publishId;
            if (m_activeUploadId == uid) m_activeUploadId.clear();
            if (!pubId.isEmpty()) afterUpload(pubId);
            return;
        }
        it->status  = QStringLiteral("retrying");
        it->errCode = code;
        it->errMsg  = msg;
        if (m_activeUploadId == uid) m_activeUploadId.clear();
        QTimer::singleShot(static_cast<int>(delayMs), this,
                           [this, uid]() { startUpload(uid); });
        return;
    }

    it->status  = QStringLiteral("error");
    it->errCode = code;
    it->errMsg  = msg;
    QDir(it->stagingDir).removeRecursively();
    const QString pubId = it->publishId;
    if (m_activeUploadId == uid) m_activeUploadId.clear();

    // Start next queued
    for (auto jt = m_uploads.begin(); jt != m_uploads.end(); ++jt) {
        if (jt->status == QStringLiteral("queued")) {
            const QString next = jt.key();
            QTimer::singleShot(0, this, [this, next]() { startUpload(next); });
            break;
        }
    }
    if (!pubId.isEmpty()) afterUpload(pubId);
}

QString ChroniclePlugin::uploadStatusJson(const QString& uid) {
    const auto it = m_uploads.constFind(uid);
    if (it == m_uploads.constEnd())
        return failJson(QStringLiteral("UNKNOWN_UPLOAD"),
                        QStringLiteral("unknown upload_id: ") + uid);
    return uploadToJson(it.value());
}

// ── Broadcast ─────────────────────────────────────────────────────────────

QString ChroniclePlugin::startBroadcasterJson() {
    if (!m_api)
        return failJson(QStringLiteral("NOT_INIT"),
                        QStringLiteral("initLogos was not called"));
    QString err;
    if (!storageReady(&err))  return failJson(QStringLiteral("STORAGE_UNAVAILABLE"), err);
    if (!deliveryReady(&err)) return failJson(QStringLiteral("DELIVERY_UNAVAILABLE"), err);
    return okJson({{QStringLiteral("started"), true},
                   {QStringLiteral("topic"),   m_broadcastTopic}});
}

QString ChroniclePlugin::broadcastEnvelopeJson(const QString& envelopeJson) {
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(envelopeJson.toUtf8(), &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject())
        return failJson(QStringLiteral("INVALID_ENVELOPE"),
                        QStringLiteral("envelopeJson must be a JSON object"));

    const QJsonObject env = doc.object();
    if (env.value(QStringLiteral("v")).toInt(-1) != 1)
        return failJson(QStringLiteral("BAD_VERSION"),
                        QStringLiteral("only envelope v=1 is supported"));
    const QString cid = env.value(QStringLiteral("cid")).toString();
    if (cid.trimmed().isEmpty())
        return failJson(QStringLiteral("EMPTY_CID"), QStringLiteral("cid is required"));
    const QString hash = env.value(QStringLiteral("metadata_hash")).toString();
    if (hash.trimmed().isEmpty())
        return failJson(QStringLiteral("MISSING_HASH"),
                        QStringLiteral("metadata_hash is required"));

    const QString dedupeKey = cid + QStringLiteral(":") + hash;
    for (const BroadcastJob& b : std::as_const(m_broadcasts)) {
        if (b.cid + QStringLiteral(":") + b.metaHash == dedupeKey
            && b.status != QStringLiteral("error")) {
            return okJson({{QStringLiteral("queued"),       true},
                           {QStringLiteral("broadcast_id"), b.id},
                           {QStringLiteral("deduped"),      true},
                           {QStringLiteral("status"),       b.status}});
        }
    }

    if (!m_api)
        return failJson(QStringLiteral("NOT_INIT"),
                        QStringLiteral("initLogos was not called"));

    BroadcastJob job;
    job.id       = QUuid::createUuid().toString(QUuid::WithoutBraces);
    job.status   = QStringLiteral("queued");
    job.cid      = cid;
    job.metaHash = hash;
    job.topic    = m_broadcastTopic;
    job.envelope = env;
    job.createdMs = QDateTime::currentMSecsSinceEpoch();
    m_broadcasts.insert(job.id, job);
    m_bcastDedupe.insert(dedupeKey);

    const QString bid = job.id;
    QTimer::singleShot(0, this, [this, bid]() { sendBroadcast(bid); });

    return okJson({{QStringLiteral("queued"),       true},
                   {QStringLiteral("broadcast_id"), bid},
                   {QStringLiteral("deduped"),      false}});
}

void ChroniclePlugin::sendBroadcast(const QString& bid) {
    auto it = m_broadcasts.find(bid);
    if (it == m_broadcasts.end() || it->status != QStringLiteral("queued")) return;

    QString err;
    if (!deliveryReady(&err)) {
        it->status  = QStringLiteral("error");
        it->errCode = QStringLiteral("DELIVERY_UNAVAILABLE");
        it->errMsg  = err;
        m_bcastDedupe.remove(it->cid + QStringLiteral(":") + it->metaHash);
        afterBroadcast(bid);
        return;
    }

    it->status = QStringLiteral("sent");
    afterBroadcast(bid);

    // Optimistic: mark sent then dispatch async
    m_deliveryClient->invokeRemoteMethodAsync(
        QStringLiteral("delivery_module"), QStringLiteral("send"),
        m_broadcastTopic,
        compact(it->envelope),
        [](QVariant) {},
        Timeout(120000));
}

QString ChroniclePlugin::broadcastStatusJson(const QString& bid) {
    const auto it = m_broadcasts.constFind(bid);
    if (it == m_broadcasts.constEnd())
        return failJson(QStringLiteral("UNKNOWN_BROADCAST"),
                        QStringLiteral("unknown broadcast_id: ") + bid);
    return broadcastToJson(it.value());
}

// ── Publish ───────────────────────────────────────────────────────────────

QString ChroniclePlugin::publishFileJson(const QString& reqJson) {
    const QJsonObject req = QJsonDocument::fromJson(reqJson.toUtf8()).object();
    const QString path    = req.value(QStringLiteral("path")).toString();
    const QString ct      = req.value(QStringLiteral("content_type")).toString();
    const QString title   = req.value(QStringLiteral("title")).toString();
    const QString desc    = req.value(QStringLiteral("description")).toString();
    const bool    bcast   = req.value(QStringLiteral("broadcast")).toBool(true);

    QStringList tags;
    if (req.contains(QStringLiteral("tags"))) {
        const QJsonArray arr = req.value(QStringLiteral("tags")).toArray();
        for (const auto& v : arr) tags.append(v.toString());
    }

    // Start the upload sub-job
    const QString upResp = uploadFileJson(path, ct, title);
    const QJsonObject upObj = QJsonDocument::fromJson(upResp.toUtf8()).object();
    if (!upObj.value(QStringLiteral("ok")).toBool(true)
        || !upObj.value(QStringLiteral("queued")).toBool()) {
        return upResp;
    }
    const QString uploadId = upObj.value(QStringLiteral("upload_id")).toString();

    const QString pubId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    PublishJob pub;
    pub.id          = pubId;
    pub.uploadId    = uploadId;
    pub.status      = QStringLiteral("uploading");
    pub.contentType = coerceContentType(ct);
    pub.title       = cleanTitle(title);
    pub.description = cleanDescription(desc);
    pub.tags        = cleanTags(tags);
    pub.wantBroadcast = bcast;
    pub.createdMs   = now;
    pub.updatedMs   = now;
    m_publishes.insert(pubId, pub);
    m_publishDedupe.insert(path + QStringLiteral(":") + title, pubId);

    // Link the upload job back to this publish
    if (auto uIt = m_uploads.find(uploadId); uIt != m_uploads.end()) {
        uIt->publishId = pubId;
    }

    return okJson({{QStringLiteral("queued"),     true},
                   {QStringLiteral("publish_id"), pubId}});
}

void ChroniclePlugin::afterUpload(const QString& pubId) {
    auto pIt = m_publishes.find(pubId);
    if (pIt == m_publishes.end()) return;

    const auto uIt = m_uploads.constFind(pIt->uploadId);
    if (uIt == m_uploads.constEnd()) return;

    if (uIt->status == QStringLiteral("error")
        || uIt->status == QStringLiteral("retrying")) {
        pIt->status  = uIt->status == QStringLiteral("retrying")
                       ? QStringLiteral("uploading") : QStringLiteral("error");
        pIt->errCode = uIt->errCode;
        pIt->errMsg  = uIt->errMsg;
        pIt->updatedMs = QDateTime::currentMSecsSinceEpoch();
        if (uIt->status != QStringLiteral("retrying")) savePublish(*pIt);
        return;
    }

    pIt->cid      = uIt->cid;
    pIt->metaHash = uIt->metaHash;
    pIt->sizeBytes = uIt->sizeBytes;
    pIt->envelope = uIt->envelope;
    pIt->updatedMs = QDateTime::currentMSecsSinceEpoch();

    if (!pIt->wantBroadcast) {
        pIt->status = QStringLiteral("done");
        savePublish(*pIt);
        return;
    }

    pIt->status = QStringLiteral("broadcasting");
    const QString bResp = broadcastEnvelopeJson(compact(pIt->envelope));
    const QJsonObject bObj = QJsonDocument::fromJson(bResp.toUtf8()).object();
    if (!bObj.value(QStringLiteral("ok")).toBool(
            bObj.value(QStringLiteral("queued")).toBool())) {
        pIt->status  = QStringLiteral("error");
        pIt->errCode = bObj.value(QStringLiteral("code")).toString();
        pIt->errMsg  = bObj.value(QStringLiteral("error")).toString();
        pIt->updatedMs = QDateTime::currentMSecsSinceEpoch();
        savePublish(*pIt);
        return;
    }
    pIt->broadcastId = bObj.value(QStringLiteral("broadcast_id")).toString();
    m_bcastToPublish.insert(pIt->broadcastId, pubId);
}

void ChroniclePlugin::afterBroadcast(const QString& bid) {
    const QString pubId = m_bcastToPublish.value(bid);
    if (pubId.isEmpty()) return;

    auto pIt = m_publishes.find(pubId);
    if (pIt == m_publishes.end()) return;

    const auto bIt = m_broadcasts.constFind(bid);
    if (bIt == m_broadcasts.constEnd()) return;

    pIt->updatedMs = QDateTime::currentMSecsSinceEpoch();
    if (bIt->status == QStringLiteral("error")) {
        pIt->status  = QStringLiteral("error");
        pIt->errCode = bIt->errCode;
        pIt->errMsg  = bIt->errMsg;
    } else {
        pIt->status = QStringLiteral("done");
    }
    savePublish(*pIt);
}

QString ChroniclePlugin::publishStatusJson(const QString& pid) {
    const auto it = m_publishes.constFind(pid);
    if (it == m_publishes.constEnd())
        return failJson(QStringLiteral("UNKNOWN_PUBLISH"),
                        QStringLiteral("unknown publish_id: ") + pid);
    return publishToJson(it.value());
}

QString ChroniclePlugin::listPublishedJson() {
    QJsonArray arr;
    for (const PublishJob& p : std::as_const(m_publishes))
        arr.append(QJsonDocument::fromJson(publishToJson(p).toUtf8()).object());
    return okJson({{QStringLiteral("items"), arr}});
}

QString ChroniclePlugin::clearPublishedJson() {
    m_publishes.clear();
    QFile::remove(publishLedgerPath());
    return okJson({});
}

// ── Anchor ────────────────────────────────────────────────────────────────

QString ChroniclePlugin::anchorCapabilitiesJson() {
    if (!m_anchorCfgLoaded) {
        m_anchorCfg = AnchorStore::load();
        m_anchorCfgLoaded = true;
    }
    QJsonArray missing;
    for (const QString& f : m_anchorCfg.missingFields()) missing.append(f);
    return okJson({{QStringLiteral("configured"),    m_anchorCfg.isReady()},
                   {QStringLiteral("missing_fields"), missing}});
}

QString ChroniclePlugin::getAnchorConfigJson() {
    if (!m_anchorCfgLoaded) {
        m_anchorCfg = AnchorStore::load();
        m_anchorCfgLoaded = true;
    }
    return okJson({{QStringLiteral("config"), m_anchorCfg.toJson()}});
}

QString ChroniclePlugin::setAnchorConfigJson(const QString& cfgJson) {
    const QJsonObject obj = QJsonDocument::fromJson(cfgJson.toUtf8()).object();
    if (obj.isEmpty())
        return failJson(QStringLiteral("INVALID_INPUT"),
                        QStringLiteral("cfgJson must be a JSON object"));
    m_anchorCfg = AnchorConfig::fromJson(obj);
    m_anchorCfgLoaded = true;
    QString err;
    if (!AnchorStore::save(m_anchorCfg, &err))
        return failJson(QStringLiteral("SAVE_FAILED"), err);
    return okJson({{QStringLiteral("config"), m_anchorCfg.toJson()}});
}

QString ChroniclePlugin::anchorBatchJson(const QString& reqJson) {
    if (!m_anchorCfgLoaded) {
        m_anchorCfg = AnchorStore::load();
        m_anchorCfgLoaded = true;
    }
    if (!m_anchorCfg.isReady())
        return failJson(QStringLiteral("ANCHOR_NOT_CONFIGURED"),
                        QStringLiteral("anchor not configured; set program_id, "
                                       "signer_account_id via setAnchorConfigJson"));

    const QJsonObject req = QJsonDocument::fromJson(reqJson.toUtf8()).object();
    const QString cid     = req.value(QStringLiteral("cid")).toString();
    const QString pubId   = req.value(QStringLiteral("publish_id")).toString();
    const QString metaHash = req.value(QStringLiteral("metadata_hash")).toString();
    if (cid.trimmed().isEmpty())
        return failJson(QStringLiteral("EMPTY_CID"), QStringLiteral("cid is required"));

    if (!m_ffi) m_ffi = new FfiClient(this);

    const QString anchorId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    const QJsonObject ffiArgs = {
        {QStringLiteral("program_id_hex"),    m_anchorCfg.programId},
        {QStringLiteral("sequencer_url"),     m_anchorCfg.sequencerUrl},
        {QStringLiteral("wallet_home"),       m_anchorCfg.walletHome},
        {QStringLiteral("signer_account_id"), m_anchorCfg.signerAccountId},
        {QStringLiteral("cids"),              QJsonArray{cid}},
        {QStringLiteral("metadata_hashes"),   QJsonArray{metaHash}},
        {QStringLiteral("anchor_timestamps"), QJsonArray{now / 1000}},
    };
    const QString resp = m_ffi->indexBatch(compact(ffiArgs));
    const QJsonObject rObj = QJsonDocument::fromJson(resp.toUtf8()).object();
    const bool ok = rObj.value(QStringLiteral("ok")).toBool();

    AnchorRecord rec;
    rec.publishId   = pubId;
    rec.cid         = cid;
    rec.metaHash    = metaHash;
    rec.attemptedMs = now;
    rec.state = ok ? QStringLiteral("confirmed") : QStringLiteral("failed");
    if (ok) rec.confirmedMs = QDateTime::currentMSecsSinceEpoch();
    else {
        rec.errCode = rObj.value(QStringLiteral("code")).toString();
        rec.errMsg  = rObj.value(QStringLiteral("error")).toString();
    }
    m_anchors.insert(cid, rec);
    saveAnchor(rec);

    if (!ok)
        return failJson(rec.errCode.isEmpty() ? QStringLiteral("ANCHOR_FAILED")
                                              : rec.errCode, rec.errMsg);
    return okJson({{QStringLiteral("anchor_id"),   anchorId},
                   {QStringLiteral("cid"),          cid},
                   {QStringLiteral("state"),        QStringLiteral("confirmed")}});
}

QString ChroniclePlugin::anchorStatusJson(const QString& cid) {
    const auto it = m_anchors.constFind(cid);
    if (it == m_anchors.constEnd())
        return failJson(QStringLiteral("NOT_FOUND"),
                        QStringLiteral("no anchor record for: ") + cid);
    return okJson(anchorToJson(it.value()));
}

QString ChroniclePlugin::lookupAnchorJson(const QString& cid) {
    if (!m_anchorCfg.isReady())
        return failJson(QStringLiteral("ANCHOR_NOT_CONFIGURED"),
                        QStringLiteral("anchor not configured"));
    if (!m_ffi) m_ffi = new FfiClient(this);
    const QJsonObject args = {
        {QStringLiteral("program_id_hex"), m_anchorCfg.programId},
        {QStringLiteral("sequencer_url"),  m_anchorCfg.sequencerUrl},
        {QStringLiteral("wallet_home"),    m_anchorCfg.walletHome},
    };
    const QString resp = m_ffi->getRegistry(compact(args));
    const QJsonObject rObj = QJsonDocument::fromJson(resp.toUtf8()).object();
    if (!rObj.value(QStringLiteral("ok")).toBool())
        return resp;
    const QJsonObject entries =
        rObj.value(QStringLiteral("entries")).toObject();
    if (!entries.contains(cid))
        return failJson(QStringLiteral("NOT_FOUND"),
                        QStringLiteral("CID not anchored: ") + cid);
    return okJson({{QStringLiteral("cid"),   cid},
                   {QStringLiteral("record"), entries.value(cid)}});
}

QString ChroniclePlugin::listAnchorsJson() {
    QJsonObject map;
    for (auto it = m_anchors.constBegin(); it != m_anchors.constEnd(); ++it)
        map.insert(it.key(), anchorToJson(it.value()));
    return okJson({{QStringLiteral("anchors"), map}});
}

QString ChroniclePlugin::clearAnchorsJson() {
    m_anchors.clear();
    QFile::remove(anchorLedgerPath());
    return okJson({});
}

QString ChroniclePlugin::initRegistryJson() {
    if (!m_anchorCfg.isReady())
        return failJson(QStringLiteral("ANCHOR_NOT_CONFIGURED"),
                        QStringLiteral("anchor not configured"));
    if (!m_ffi) m_ffi = new FfiClient(this);
    const QJsonObject args = {
        {QStringLiteral("program_id_hex"),    m_anchorCfg.programId},
        {QStringLiteral("sequencer_url"),     m_anchorCfg.sequencerUrl},
        {QStringLiteral("wallet_home"),       m_anchorCfg.walletHome},
        {QStringLiteral("signer_account_id"), m_anchorCfg.signerAccountId},
    };
    return m_ffi->initRegistry(compact(args));
}

QString ChroniclePlugin::getRegistryJson() {
    if (!m_anchorCfg.isReady())
        return failJson(QStringLiteral("ANCHOR_NOT_CONFIGURED"),
                        QStringLiteral("anchor not configured"));
    if (!m_ffi) m_ffi = new FfiClient(this);
    const QJsonObject args = {
        {QStringLiteral("program_id_hex"), m_anchorCfg.programId},
        {QStringLiteral("sequencer_url"),  m_anchorCfg.sequencerUrl},
        {QStringLiteral("wallet_home"),    m_anchorCfg.walletHome},
    };
    return m_ffi->getRegistry(compact(args));
}

QString ChroniclePlugin::setBroadcastTopic(const QString& topic) {
    m_broadcastTopic = topic.isEmpty()
                       ? QString::fromLatin1(kDefaultTopic) : topic;
    return okJson({{QStringLiteral("topic"), m_broadcastTopic}});
}

QString ChroniclePlugin::getBroadcastTopic() {
    return okJson({{QStringLiteral("topic"), m_broadcastTopic}});
}

// ── Persistence helpers ────────────────────────────────────────────────────

QString ChroniclePlugin::publishLedgerPath() const {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
           + QStringLiteral("/chronicle/publish-ledger.json");
}

void ChroniclePlugin::savePublish(const PublishJob& job) {
    const QString path = publishLedgerPath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    QJsonObject all;
    if (f.open(QIODevice::ReadOnly | QIODevice::Text))
        all = QJsonDocument::fromJson(f.readAll()).object();
    f.close();
    all.insert(job.id, QJsonDocument::fromJson(publishToJson(job).toUtf8()).object());
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        f.write(QJsonDocument(all).toJson(QJsonDocument::Compact));
}

void ChroniclePlugin::loadPublishLedger() {
    QFile f(publishLedgerPath());
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return;
    const QJsonObject all = QJsonDocument::fromJson(f.readAll()).object();
    for (auto it = all.constBegin(); it != all.constEnd(); ++it) {
        const QJsonObject obj = it.value().toObject();
        PublishJob job;
        job.id          = it.key();
        job.status      = obj.value(QStringLiteral("status")).toString();
        job.cid         = obj.value(QStringLiteral("cid")).toString();
        job.metaHash    = obj.value(QStringLiteral("metadata_hash")).toString();
        job.contentType = obj.value(QStringLiteral("content_type")).toString();
        job.title       = obj.value(QStringLiteral("title")).toString();
        job.description = obj.value(QStringLiteral("description")).toString();
        job.sizeBytes   = obj.value(QStringLiteral("size_bytes")).toDouble(0);
        job.createdMs   = static_cast<qint64>(obj.value(QStringLiteral("created_at_ms")).toDouble(0));
        job.updatedMs   = static_cast<qint64>(obj.value(QStringLiteral("updated_at_ms")).toDouble(0));
        m_publishes.insert(job.id, job);
    }
}

QString ChroniclePlugin::anchorLedgerPath() const {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
           + QStringLiteral("/chronicle/anchor-ledger.json");
}

void ChroniclePlugin::saveAnchor(const AnchorRecord& rec) {
    const QString path = anchorLedgerPath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    QJsonObject all;
    if (f.open(QIODevice::ReadOnly | QIODevice::Text))
        all = QJsonDocument::fromJson(f.readAll()).object();
    f.close();
    all.insert(rec.cid, anchorToJson(rec));
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        f.write(QJsonDocument(all).toJson(QJsonDocument::Compact));
}

void ChroniclePlugin::loadAnchorLedger() {
    QFile f(anchorLedgerPath());
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return;
    const QJsonObject all = QJsonDocument::fromJson(f.readAll()).object();
    for (auto it = all.constBegin(); it != all.constEnd(); ++it) {
        const QJsonObject obj = it.value().toObject();
        AnchorRecord rec;
        rec.cid         = it.key();
        rec.state       = obj.value(QStringLiteral("state")).toString();
        rec.publishId   = obj.value(QStringLiteral("publish_id")).toString();
        rec.metaHash    = obj.value(QStringLiteral("metadata_hash")).toString();
        rec.errCode     = obj.value(QStringLiteral("error_code")).toString();
        rec.errMsg      = obj.value(QStringLiteral("error")).toString();
        rec.attemptedMs = static_cast<qint64>(obj.value(QStringLiteral("attempted_at_ms")).toDouble(0));
        rec.confirmedMs = static_cast<qint64>(obj.value(QStringLiteral("confirmed_at_ms")).toDouble(0));
        m_anchors.insert(rec.cid, rec);
    }
    m_anchorsLoaded = true;
}

// ── Serialisation ─────────────────────────────────────────────────────────

QString ChroniclePlugin::uploadToJson(const UploadJob& j) const {
    QJsonObject obj;
    obj.insert(QStringLiteral("upload_id"),   j.id);
    obj.insert(QStringLiteral("status"),      j.status);
    obj.insert(QStringLiteral("path"),        j.path);
    obj.insert(QStringLiteral("content_type"), j.contentType);
    obj.insert(QStringLiteral("title"),       j.title);
    obj.insert(QStringLiteral("size_bytes"),  j.sizeBytes);
    obj.insert(QStringLiteral("cid"),         j.cid);
    obj.insert(QStringLiteral("metadata_hash"), j.metaHash);
    if (!j.errCode.isEmpty()) {
        obj.insert(QStringLiteral("error_code"), j.errCode);
        obj.insert(QStringLiteral("error"),      j.errMsg);
    }
    obj.insert(QStringLiteral("ok"), true);
    return compact(obj);
}

QString ChroniclePlugin::broadcastToJson(const BroadcastJob& j) const {
    QJsonObject obj;
    obj.insert(QStringLiteral("broadcast_id"),  j.id);
    obj.insert(QStringLiteral("status"),        j.status);
    obj.insert(QStringLiteral("cid"),           j.cid);
    obj.insert(QStringLiteral("metadata_hash"), j.metaHash);
    obj.insert(QStringLiteral("topic"),         j.topic);
    if (!j.errCode.isEmpty()) {
        obj.insert(QStringLiteral("error_code"), j.errCode);
        obj.insert(QStringLiteral("error"),      j.errMsg);
    }
    obj.insert(QStringLiteral("ok"), true);
    return compact(obj);
}

QString ChroniclePlugin::publishToJson(const PublishJob& j) const {
    QJsonObject obj;
    obj.insert(QStringLiteral("publish_id"),    j.id);
    obj.insert(QStringLiteral("status"),        j.status);
    obj.insert(QStringLiteral("cid"),           j.cid);
    obj.insert(QStringLiteral("metadata_hash"), j.metaHash);
    obj.insert(QStringLiteral("content_type"),  j.contentType);
    obj.insert(QStringLiteral("title"),         j.title);
    obj.insert(QStringLiteral("description"),   j.description);
    obj.insert(QStringLiteral("size_bytes"),    j.sizeBytes);
    obj.insert(QStringLiteral("created_at_ms"), j.createdMs);
    obj.insert(QStringLiteral("updated_at_ms"), j.updatedMs);
    if (!j.errCode.isEmpty()) {
        obj.insert(QStringLiteral("error_code"), j.errCode);
        obj.insert(QStringLiteral("error"),      j.errMsg);
    }
    obj.insert(QStringLiteral("ok"), true);
    return compact(obj);
}

QJsonObject ChroniclePlugin::anchorToJson(const AnchorRecord& r) const {
    QJsonObject obj;
    obj.insert(QStringLiteral("cid"),           r.cid);
    obj.insert(QStringLiteral("state"),         r.state);
    obj.insert(QStringLiteral("publish_id"),    r.publishId);
    obj.insert(QStringLiteral("metadata_hash"), r.metaHash);
    obj.insert(QStringLiteral("attempted_at_ms"), r.attemptedMs);
    obj.insert(QStringLiteral("confirmed_at_ms"), r.confirmedMs);
    if (!r.errCode.isEmpty()) {
        obj.insert(QStringLiteral("error_code"), r.errCode);
        obj.insert(QStringLiteral("error"),      r.errMsg);
    }
    return obj;
}

QString ChroniclePlugin::errJson(const QString& code, const QString& msg) const {
    return failJson(code, msg);
}
