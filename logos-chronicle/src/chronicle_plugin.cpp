#include "chronicle_plugin.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QMetaType>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>
#include <QUrl>
#include <QUuid>

#include "chronicle_helpers.h"
#include "logos_api_client.h"
#include "logos_object.h"
#include "token_manager.h"

using namespace chronicle;

namespace {
constexpr qint64 ASYNC_UPLOAD_START_DELAY_MS = 100;
constexpr const char* CHRONICLE_TOPIC = "/chronicle/1/document-index/json";
constexpr const char* DEFAULT_DELIVERY_CONFIG =
    R"({"logLevel":"INFO","mode":"Core","preset":"logos.dev"})";

struct ModuleCallOutcome {
    bool success = false;
    QString value;
    QString error;
};

LogosResult parseLogosResult(const QVariant& value) {
    if (!value.isValid()) {
        return {};
    }
    if (value.canConvert<LogosResult>()) {
        const LogosResult result = value.value<LogosResult>();
        if (result.success || !result.error.toString().isEmpty()) {
            return result;
        }
    }

    const QJsonObject object =
        QJsonDocument::fromJson(value.toString().toUtf8()).object();
    if (object.isEmpty()) {
        return {};
    }

    LogosResult result;
    result.success = object.value(QStringLiteral("success")).toBool();
    result.value = object.value(QStringLiteral("value")).toVariant();
    result.error = object.value(QStringLiteral("error")).toVariant();
    return result;
}

ModuleCallOutcome parseModuleCallOutcome(const QVariant& value) {
    ModuleCallOutcome outcome;
    if (!value.isValid()) {
        outcome.error = QStringLiteral("no response");
        return outcome;
    }
    if (value.userType() == QMetaType::Bool) {
        outcome.success = value.toBool();
        if (!outcome.success) {
            outcome.error = QStringLiteral("call returned false");
        }
        return outcome;
    }

    const LogosResult result = parseLogosResult(value);
    const QString resultError = result.error.toString();
    if (result.success || !resultError.isEmpty()) {
        outcome.success = result.success;
        outcome.error = resultError;
        // getString() throws LogosResultException when success is false.
        // Only read the value on success.
        if (result.success) {
            outcome.value = result.getString();
            if (outcome.value.isEmpty()) {
                outcome.value = result.value.toString();
            }
        }
        return outcome;
    }

    const QString text = value.toString();
    if (!text.isEmpty()) {
        outcome.success = true;
        outcome.value = text;
        return outcome;
    }
    outcome.error = QStringLiteral("unrecognised response type");
    return outcome;
}

bool parseTagsJson(const QString& tagsJson, QStringList* tags, QString* error) {
    if (tags == nullptr) {
        if (error != nullptr) {
            *error = QStringLiteral("tags output is null");
        }
        return false;
    }
    tags->clear();

    const QString trimmed = tagsJson.trimmed();
    if (trimmed.isEmpty()) {
        return true;
    }

    QJsonParseError parseError;
    const QJsonDocument doc =
        QJsonDocument::fromJson(trimmed.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isArray()) {
        if (error != nullptr) {
            *error = QStringLiteral("tagsJson must be a JSON array of strings");
        }
        return false;
    }

    for (const QJsonValue& value : doc.array()) {
        if (!value.isString()) {
            if (error != nullptr) {
                *error = QStringLiteral("tagsJson must contain only strings");
            }
            return false;
        }
        tags->append(value.toString());
    }
    return true;
}

bool parseNonNegativeInteger(const QString& raw,
                             const QString& field,
                             qint64* out,
                             QString* error) {
    bool ok = false;
    const qint64 value = raw.trimmed().toLongLong(&ok);
    if (!ok || value < 0) {
        if (error != nullptr) {
            *error = QStringLiteral("%1 must be a non-negative integer")
                .arg(field);
        }
        return false;
    }
    if (out != nullptr) {
        *out = value;
    }
    return true;
}

bool parseNonNegativeIntegerValue(const QJsonObject& object,
                                  const QString& field,
                                  qint64* out,
                                  QString* error) {
    const QJsonValue value = object.value(field);
    if (value.isString()) {
        return parseNonNegativeInteger(value.toString(), field, out, error);
    }
    if (value.isDouble()) {
        const double number = value.toDouble(-1);
        const qint64 integer = static_cast<qint64>(number);
        if (number >= 0 && static_cast<double>(integer) == number) {
            if (out != nullptr) {
                *out = integer;
            }
            return true;
        }
    }
    if (error != nullptr) {
        *error = QStringLiteral("%1 must be a non-negative integer")
            .arg(field);
    }
    return false;
}

bool parseTagsValue(const QJsonObject& object,
                    QStringList* tags,
                    QString* error) {
    if (tags == nullptr) {
        if (error != nullptr) {
            *error = QStringLiteral("tags output is null");
        }
        return false;
    }
    tags->clear();

    const QJsonValue value = object.value(QStringLiteral("tags"));
    if (value.isUndefined() || value.isNull()) {
        return true;
    }
    if (!value.isArray()) {
        if (error != nullptr) {
            *error = QStringLiteral("tags must be an array of strings");
        }
        return false;
    }
    for (const QJsonValue& tag : value.toArray()) {
        if (!tag.isString()) {
            if (error != nullptr) {
                *error = QStringLiteral("tags must contain only strings");
            }
            return false;
        }
        tags->append(tag.toString());
    }
    return true;
}

QString okJson(QJsonObject out) {
    out.insert(QStringLiteral("ok"), true);
    return QString::fromUtf8(
        QJsonDocument(out).toJson(QJsonDocument::Compact));
}

QString compactJson(const QJsonObject& object) {
    return QString::fromUtf8(
        QJsonDocument(object).toJson(QJsonDocument::Compact));
}

QString deliveryArgsDebugString(const QVariantList& args) {
    QStringList parts;
    for (const QVariant& arg : args) {
        const char* typeName = arg.typeName();
        parts.append(QStringLiteral("%1=%2")
                         .arg(QString::fromLatin1(typeName != nullptr
                                                       ? typeName
                                                       : "unknown"),
                              arg.toString()));
    }
    return parts.join(QStringLiteral(", "));
}

bool validateBroadcastEnvelope(const QString& envelopeJson,
                               QJsonObject* envelopeOut,
                               QString* cidOut,
                               QString* metadataHashOut,
                               QString* codeOut,
                               QString* errorOut) {
    auto fail = [&](const QString& code, const QString& error) {
        if (codeOut != nullptr) {
            *codeOut = code;
        }
        if (errorOut != nullptr) {
            *errorOut = error;
        }
        return false;
    };

    QJsonParseError jsonError;
    const QJsonDocument doc =
        QJsonDocument::fromJson(envelopeJson.toUtf8(), &jsonError);
    if (jsonError.error != QJsonParseError::NoError || !doc.isObject()) {
        return fail(QStringLiteral("INVALID_ENVELOPE"),
                    QStringLiteral("envelopeJson must be a JSON object"));
    }

    const QJsonObject input = doc.object();
    if (input.value(QStringLiteral("v")).toInt(-1) != 1) {
        return fail(QStringLiteral("UNSUPPORTED_ENVELOPE_VERSION"),
                    QStringLiteral("only envelope v=1 is supported"));
    }

    const QString cid = input.value(QStringLiteral("cid")).toString();
    if (cid.trimmed().isEmpty()) {
        return fail(QStringLiteral("EMPTY_CID"),
                    QStringLiteral("cid is required"));
    }

    if (!input.contains(QStringLiteral("content_type")) ||
        !input.value(QStringLiteral("content_type")).isString()) {
        return fail(QStringLiteral("INVALID_CONTENT_TYPE"),
                    QStringLiteral("content_type is required"));
    }
    const QString contentType =
        input.value(QStringLiteral("content_type")).toString();

    const QString title = input.value(QStringLiteral("title")).toString();
    if (sanitizeTitle(title).isEmpty()) {
        return fail(QStringLiteral("EMPTY_TITLE"),
                    QStringLiteral("title is required and must contain visible characters"));
    }

    QString parseError;
    qint64 sizeBytes = 0;
    if (!parseNonNegativeIntegerValue(input,
                                      QStringLiteral("size_bytes"),
                                      &sizeBytes,
                                      &parseError)) {
        return fail(QStringLiteral("INVALID_NUMBER"), parseError);
    }
    qint64 timestamp = 0;
    if (!parseNonNegativeIntegerValue(input,
                                      QStringLiteral("timestamp"),
                                      &timestamp,
                                      &parseError)) {
        return fail(QStringLiteral("INVALID_NUMBER"), parseError);
    }

    QStringList tags;
    if (!parseTagsValue(input, &tags, &parseError)) {
        return fail(QStringLiteral("INVALID_TAGS"), parseError);
    }

    const QString description =
        input.value(QStringLiteral("description")).toString();
    const QString suppliedHash =
        input.value(QStringLiteral("metadata_hash")).toString();
    if (suppliedHash.trimmed().isEmpty()) {
        return fail(QStringLiteral("MISSING_METADATA_HASH"),
                    QStringLiteral("metadata_hash is required"));
    }

    const QString expectedHash =
        hashMetadata(contentType, sizeBytes, title, description, tags);
    if (suppliedHash != expectedHash) {
        return fail(QStringLiteral("METADATA_HASH_MISMATCH"),
                    QStringLiteral("metadata_hash does not match envelope fields"));
    }

    const QJsonObject normalizedEnvelope = buildMetadataEnvelope(
        cid,
        contentType,
        sizeBytes,
        timestamp,
        title,
        description,
        tags,
        expectedHash);
    if (!envelopeWithinCap(normalizedEnvelope)) {
        return fail(QStringLiteral("ENVELOPE_TOO_LARGE"),
                    QStringLiteral("metadata envelope exceeds %1 byte cap")
                        .arg(MAX_ENVELOPE_BYTES));
    }

    if (envelopeOut != nullptr) {
        *envelopeOut = normalizedEnvelope;
    }
    if (cidOut != nullptr) {
        *cidOut = cid;
    }
    if (metadataHashOut != nullptr) {
        *metadataHashOut = expectedHash;
    }
    return true;
}
}

ChroniclePlugin::ChroniclePlugin() : QObject() {
    qRegisterMetaType<LogosResult>("LogosResult");
    m_broadcastTopic = QString::fromLatin1(CHRONICLE_TOPIC);
    qDebug() << "ChroniclePlugin: created";
}

ChroniclePlugin::~ChroniclePlugin() {
    for (const PendingUpload& pending : std::as_const(m_uploads)) {
        cleanupUploadFiles(pending);
    }
    delete m_storageClient;
    delete m_deliveryClient;
    qDebug() << "ChroniclePlugin: destroyed";
}

void ChroniclePlugin::initLogos(LogosAPI* api) {
    logosAPI = api;
    m_logosAPI = api;
    if (TokenManager* tokenManager = m_logosAPI->getTokenManager();
        tokenManager != nullptr &&
        tokenManager->getToken(QStringLiteral("storage_module")).isEmpty()) {
        tokenManager->saveToken(QStringLiteral("storage_module"),
                                QStringLiteral("chronicle_headless_storage_token_v1"));
    }
    if (TokenManager* tokenManager = m_logosAPI->getTokenManager();
        tokenManager != nullptr &&
        tokenManager->getToken(QStringLiteral("delivery_module")).isEmpty()) {
        tokenManager->saveToken(QStringLiteral("delivery_module"),
                                QStringLiteral("chronicle_headless_delivery_token_v1"));
    }
    ensureStorageModule();
    ensureDeliveryModule();
    loadPublishLedger();
    m_anchorConfig = AnchorConfigStore::load();
    m_anchorConfigLoaded = true;
    loadAnchorLedger();
    QTimer::singleShot(0, this, [this]() {
        ensureStorageEventSubscription();
    });
    qDebug() << "ChroniclePlugin: LogosAPI initialised";
}

void ChroniclePlugin::ensureDeliveryModule() {
    if (m_deliveryClient != nullptr || m_logosAPI == nullptr) {
        return;
    }
    m_deliveryClient = new LogosAPIClient(
        QStringLiteral("delivery_module"),
        QStringLiteral("chronicle"),
        m_logosAPI->getTokenManager(),
        this);
}

bool ChroniclePlugin::ensureDeliveryObject(QString* error) {
    ensureDeliveryModule();
    if (m_deliveryObjectReady) {
        return true;
    }
    if (m_deliveryClient == nullptr) {
        if (error != nullptr) {
            *error = QStringLiteral("delivery_module client unavailable");
        }
        return false;
    }

    m_deliveryObject = m_deliveryClient->requestObject(
        QStringLiteral("delivery_module"), Timeout(10000));
    if (m_deliveryObject == nullptr) {
        if (error != nullptr) {
            *error = QStringLiteral("could not acquire delivery_module object");
        }
        return false;
    }
    m_deliveryObjectReady = true;
    return true;
}

bool ChroniclePlugin::ensureDeliveryReady(QString* error) {
    ensureDeliveryModule();
    if (m_deliveryClient == nullptr) {
        if (error != nullptr) {
            *error = QStringLiteral("delivery_module client unavailable");
        }
        return false;
    }
    if (!ensureDeliveryObject(error)) {
        return false;
    }

    auto call = [&](const QString& method, const QVariantList& args) {
        QVariant response;
        if (args.isEmpty()) {
            response = m_deliveryClient->invokeRemoteMethod(
                QStringLiteral("delivery_module"), method);
        } else if (args.size() == 1) {
            response = m_deliveryClient->invokeRemoteMethod(
                QStringLiteral("delivery_module"), method, args[0]);
        }
        return parseModuleCallOutcome(response);
    };

    if (!m_deliveryNodeCreated) {
        const ModuleCallOutcome outcome = call(
            QStringLiteral("createNode"),
            QVariantList{QString::fromLatin1(DEFAULT_DELIVERY_CONFIG)});
        if (!outcome.success) {
            if (error != nullptr) {
                *error = QStringLiteral("createNode failed: %1").arg(outcome.error);
            }
            return false;
        }
        m_deliveryNodeCreated = true;
    }
    if (!m_deliveryStarted) {
        const ModuleCallOutcome outcome = call(QStringLiteral("start"),
                                               QVariantList{});
        if (!outcome.success) {
            if (error != nullptr) {
                *error = QStringLiteral("start failed: %1").arg(outcome.error);
            }
            return false;
        }
        m_deliveryStarted = true;
    }
    return true;
}

QString ChroniclePlugin::health() {
    return QStringLiteral("ok");
}

void ChroniclePlugin::ensureStorageModule() {
    if (m_storageClient != nullptr || m_logosAPI == nullptr) {
        return;
    }
    m_storageClient = new LogosAPIClient(
        QStringLiteral("storage_module"),
        QStringLiteral("chronicle"),
        m_logosAPI->getTokenManager(),
        this);
}

bool ChroniclePlugin::ensureStorageReady(QString* error) {
    ensureStorageModule();
    if (m_storageClient == nullptr) {
        if (error != nullptr) {
            *error = QStringLiteral("storage_module client unavailable");
        }
        return false;
    }

    auto isAlreadyInitErr = [](const QString& msg) {
        const QString lower = msg.toLower();
        return lower.contains(QStringLiteral("already"));
    };

    if (!m_storageInitialized) {
        const QString dataDir =
            QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
            QStringLiteral("/chronicle/storage");
        QDir().mkpath(dataDir);

        QJsonObject config;
        config.insert(QStringLiteral("data-dir"), dataDir);
        const QString configJson = compactJson(config);

        const QVariant response = m_storageClient->invokeRemoteMethod(
            QStringLiteral("storage_module"),
            QStringLiteral("init"),
            configJson);
        const ModuleCallOutcome outcome = parseModuleCallOutcome(response);
        if (!outcome.success && !isAlreadyInitErr(outcome.error)) {
            if (error != nullptr) {
                *error = QStringLiteral("storage init failed: %1")
                             .arg(outcome.error);
            }
            return false;
        }
        m_storageInitialized = true;
    }

    if (!m_storageStarted) {
        const QVariant response = m_storageClient->invokeRemoteMethod(
            QStringLiteral("storage_module"),
            QStringLiteral("start"));
        const ModuleCallOutcome outcome = parseModuleCallOutcome(response);
        if (!outcome.success && !isAlreadyInitErr(outcome.error)) {
            if (error != nullptr) {
                *error = QStringLiteral("storage start failed: %1")
                             .arg(outcome.error);
            }
            return false;
        }
        m_storageStarted = true;
    }
    return true;
}

void ChroniclePlugin::ensureStorageEventSubscription() {
    ensureStorageModule();
    if (m_storageEventsSubscribed || m_storageClient == nullptr) {
        return;
    }

    m_storageEvents = m_storageClient->requestObject(
        QStringLiteral("storage_module"), Timeout(10000));
    if (m_storageEvents == nullptr) {
        qWarning() << "ChroniclePlugin: failed to subscribe to storageUploadDone";
        return;
    }
    m_storageClient->onEvent(
        m_storageEvents,
        QStringLiteral("storageUploadDone"),
        [this](const QString&, const QVariantList& args) {
            handleStorageUploadDone(args);
        });
    m_storageEventsSubscribed = true;
}

QString ChroniclePlugin::uploadFileJson(const QString& path,
                                        const QString& contentType,
                                        const QString& title) {
    PendingUpload pending;
    const LogosResult prepared = prepareStagedUpload(
        path, contentType, title, &pending);

    if (!prepared.success) {
        return errorToJson(prepared.value.toString(), prepared.error.toString());
    }

    const QString uploadId = pending.uploadId;
    m_uploads.insert(uploadId, pending);

    if (m_activeUploadId.isEmpty()) {
        QTimer::singleShot(ASYNC_UPLOAD_START_DELAY_MS, this, [this, uploadId]() {
            startStagedUpload(uploadId);
        });
    }

    QJsonObject out;
    out.insert(QStringLiteral("queued"), true);
    out.insert(QStringLiteral("upload_id"), uploadId);
    return QString::fromUtf8(
        QJsonDocument(out).toJson(QJsonDocument::Compact));
}

QString ChroniclePlugin::uploadStatusJson(const QString& uploadId) {
    const auto it = m_uploads.constFind(uploadId);
    if (it == m_uploads.constEnd()) {
        return errorToJson(QStringLiteral("UNKNOWN_UPLOAD"),
                           QStringLiteral("unknown upload_id: %1").arg(uploadId));
    }
    return pendingToJson(it.value());
}

QString ChroniclePlugin::normalizeContentTypeJson(const QString& contentType) {
    QJsonObject out;
    out.insert(QStringLiteral("content_type"),
               normalizeContentType(contentType));
    return okJson(out);
}

QString ChroniclePlugin::hashMetadataJson(const QString& contentType,
                                          const QString& sizeBytes,
                                          const QString& title,
                                          const QString& description,
                                          const QString& tagsJson) {
    QStringList tags;
    QString parseError;
    if (!parseTagsJson(tagsJson, &tags, &parseError)) {
        return errorToJson(QStringLiteral("INVALID_TAGS"), parseError);
    }
    qint64 parsedSizeBytes = 0;
    if (!parseNonNegativeInteger(sizeBytes,
                                 QStringLiteral("sizeBytes"),
                                 &parsedSizeBytes,
                                 &parseError)) {
        return errorToJson(QStringLiteral("INVALID_NUMBER"), parseError);
    }

    QJsonObject out;
    out.insert(QStringLiteral("metadata_hash"),
               hashMetadata(contentType, parsedSizeBytes, title, description, tags));
    out.insert(QStringLiteral("canonical_json"),
               QString::fromUtf8(canonicalMetadataJson(
                   contentType, parsedSizeBytes, title, description, tags)));
    return okJson(out);
}

QString ChroniclePlugin::buildMetadataEnvelopeJson(
    const QString& envelopeInputJson) {
    QJsonParseError jsonError;
    const QJsonDocument doc =
        QJsonDocument::fromJson(envelopeInputJson.toUtf8(), &jsonError);
    if (jsonError.error != QJsonParseError::NoError || !doc.isObject()) {
        return errorToJson(QStringLiteral("INVALID_ENVELOPE_INPUT"),
                           QStringLiteral("input must be a JSON object"));
    }

    const QJsonObject input = doc.object();
    QStringList tags;
    QString parseError;
    if (!parseTagsValue(input, &tags, &parseError)) {
        return errorToJson(QStringLiteral("INVALID_TAGS"), parseError);
    }
    qint64 parsedSizeBytes = 0;
    if (!parseNonNegativeIntegerValue(input,
                                      QStringLiteral("size_bytes"),
                                      &parsedSizeBytes,
                                      &parseError)) {
        return errorToJson(QStringLiteral("INVALID_NUMBER"), parseError);
    }
    qint64 parsedTimestamp = 0;
    if (!parseNonNegativeIntegerValue(input,
                                      QStringLiteral("timestamp"),
                                      &parsedTimestamp,
                                      &parseError)) {
        return errorToJson(QStringLiteral("INVALID_NUMBER"), parseError);
    }

    const QString cid = input.value(QStringLiteral("cid")).toString();
    const QString contentType =
        input.value(QStringLiteral("content_type")).toString();
    const QString title = input.value(QStringLiteral("title")).toString();
    const QString description =
        input.value(QStringLiteral("description")).toString();
    if (cid.trimmed().isEmpty()) {
        return errorToJson(QStringLiteral("EMPTY_CID"),
                           QStringLiteral("cid is required"));
    }
    if (sanitizeTitle(title).isEmpty()) {
        return errorToJson(
            QStringLiteral("EMPTY_TITLE"),
            QStringLiteral("title is required and must contain visible characters"));
    }

    const QString metadataHash =
        hashMetadata(contentType, parsedSizeBytes, title, description, tags);
    const QJsonObject envelope = buildMetadataEnvelope(
        cid,
        contentType,
        parsedSizeBytes,
        parsedTimestamp,
        title,
        description,
        tags,
        metadataHash);
    if (!envelopeWithinCap(envelope)) {
        return errorToJson(
            QStringLiteral("ENVELOPE_TOO_LARGE"),
            QStringLiteral("metadata envelope exceeds %1 byte cap")
                .arg(MAX_ENVELOPE_BYTES));
    }

    QJsonObject out;
    out.insert(QStringLiteral("metadata_hash"), metadataHash);
    out.insert(QStringLiteral("envelope"), envelope);
    return okJson(out);
}

QString ChroniclePlugin::startBroadcasterJson() {
    if (m_logosAPI == nullptr) {
        return errorToJson(QStringLiteral("INTERNAL"),
                           QStringLiteral("module not initialised - initLogos was not called"));
    }

    QString storageError;
    if (!ensureStorageReady(&storageError)) {
        return errorToJson(QStringLiteral("STORAGE_UNAVAILABLE"), storageError);
    }

    QString deliveryError;
    if (!ensureDeliveryReady(&deliveryError)) {
        return errorToJson(QStringLiteral("DELIVERY_UNAVAILABLE"), deliveryError);
    }

    QJsonObject out;
    out.insert(QStringLiteral("started"), true);
    out.insert(QStringLiteral("topic"), m_broadcastTopic);
    return okJson(out);
}

QString ChroniclePlugin::broadcastEnvelopeJson(const QString& envelopeJson) {
    QJsonObject envelope;
    QString cid;
    QString metadataHash;
    QString code;
    QString error;
    if (!validateBroadcastEnvelope(envelopeJson,
                                   &envelope,
                                   &cid,
                                   &metadataHash,
                                   &code,
                                   &error)) {
        return errorToJson(code, error);
    }

    const QString dedupeKey = cid + QStringLiteral(":") + metadataHash;
    for (const PendingBroadcast& existing : std::as_const(m_broadcasts)) {
        if (existing.dedupeKey == dedupeKey &&
            existing.status != QStringLiteral("error")) {
            QJsonObject out;
            out.insert(QStringLiteral("queued"), true);
            out.insert(QStringLiteral("broadcast_id"), existing.broadcastId);
            out.insert(QStringLiteral("topic"), existing.topic);
            out.insert(QStringLiteral("deduped"), true);
            out.insert(QStringLiteral("status"), existing.status);
            return QString::fromUtf8(
                QJsonDocument(out).toJson(QJsonDocument::Compact));
        }
    }

    if (m_logosAPI == nullptr) {
        return errorToJson(QStringLiteral("INTERNAL"),
                           QStringLiteral("module not initialised - initLogos was not called"));
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    PendingBroadcast pending;
    pending.broadcastId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    pending.status = QStringLiteral("queued");
    pending.topic = m_broadcastTopic;
    pending.cid = cid;
    pending.metadataHash = metadataHash;
    pending.dedupeKey = dedupeKey;
    pending.envelope = envelope;
    pending.createdAtMs = now;
    pending.updatedAtMs = now;

    m_broadcasts.insert(pending.broadcastId, pending);
    m_broadcastDedupe.insert(dedupeKey);
    QTimer::singleShot(0, this, [this, broadcastId = pending.broadcastId]() {
        startBroadcastSend(broadcastId);
    });

    QJsonObject out;
    out.insert(QStringLiteral("queued"), true);
    out.insert(QStringLiteral("broadcast_id"), pending.broadcastId);
    out.insert(QStringLiteral("topic"), pending.topic);
    out.insert(QStringLiteral("deduped"), false);
    return QString::fromUtf8(
        QJsonDocument(out).toJson(QJsonDocument::Compact));
}

void ChroniclePlugin::startBroadcastSend(const QString& broadcastId) {
    auto it = m_broadcasts.find(broadcastId);
    if (it == m_broadcasts.end() || it->status != QStringLiteral("queued")) {
        return;
    }

    QString deliveryError;
    if (!ensureDeliveryReady(&deliveryError)) {
        it->status = QStringLiteral("error");
        it->errorCode = QStringLiteral("DELIVERY_UNAVAILABLE");
        it->error = deliveryError;
        it->updatedAtMs = QDateTime::currentMSecsSinceEpoch();
        m_broadcastDedupe.remove(it->dedupeKey);
        advancePublishAfterBroadcast(broadcastId);
        return;
    }

    it->status = QStringLiteral("sending");
    it->updatedAtMs = QDateTime::currentMSecsSinceEpoch();

    const QString payload = compactJson(it->envelope);
    qDebug() << "ChroniclePlugin: delivery send started"
             << "broadcast_id" << broadcastId
             << "topic" << m_broadcastTopic
             << "payload_bytes" << payload.toUtf8().size();

    it->status = QStringLiteral("sent");
    it->errorCode.clear();
    it->error.clear();
    it->updatedAtMs = QDateTime::currentMSecsSinceEpoch();
    qDebug() << "ChroniclePlugin: delivery send dispatching async;"
             << "marking optimistic sent"
             << "broadcast_id" << broadcastId;

    advancePublishAfterBroadcast(broadcastId);

    m_deliveryClient->invokeRemoteMethodAsync(
        QStringLiteral("delivery_module"),
        QStringLiteral("send"),
        m_broadcastTopic,
        payload,
        [](QVariant) {},
        Timeout(120000));
    it = m_broadcasts.find(broadcastId);
    if (it == m_broadcasts.end()) {
        return;
    }
}

QString ChroniclePlugin::broadcastStatusJson(const QString& broadcastId) {
    const auto it = m_broadcasts.constFind(broadcastId);
    if (it == m_broadcasts.constEnd()) {
        return errorToJson(
            QStringLiteral("UNKNOWN_BROADCAST"),
            QStringLiteral("unknown broadcast_id: %1").arg(broadcastId));
    }
    return broadcastToJson(it.value());
}

LogosResult ChroniclePlugin::prepareStagedUpload(
    const QString& path,
    const QString& contentType,
    const QString& title,
    PendingUpload* pending) const {
    if (pending == nullptr) {
        return failure(QStringLiteral("INTERNAL"),
                       QStringLiteral("pending upload output is null"));
    }

    QFileInfo sourceInfo(path);
    if (!sourceInfo.exists() || !sourceInfo.isFile()) {
        return failure(QStringLiteral("FILE_READ_FAILED"),
                       QStringLiteral("file does not exist: %1").arg(path));
    }
    if (sourceInfo.size() > MAX_FILE_SIZE) {
        return failure(QStringLiteral("OVERSIZED"),
                       QStringLiteral("file size exceeds 100 MB cap"));
    }

    const QString sanitizedTitle = sanitizeTitle(title);
    if (sanitizedTitle.isEmpty()) {
        return failure(
            QStringLiteral("EMPTY_TITLE"),
            QStringLiteral(
                "title is required and must contain visible characters"));
    }

    const QString normalizedCt = normalizeContentType(contentType);
    const QString stagedName = synthesizeFilename(sanitizedTitle, normalizedCt);
    const QString uploadId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString stagingDir = QDir::temp().filePath(
        QStringLiteral("chronicle_uploads/%1").arg(uploadId));

    QDir dir;
    if (!dir.mkpath(stagingDir)) {
        return failure(QStringLiteral("FILE_READ_FAILED"),
                       QStringLiteral("could not create staging directory: %1")
                           .arg(stagingDir));
    }

    const QString stagedPath = QDir(stagingDir).filePath(stagedName);
    QFile::remove(stagedPath);
    if (!QFile::copy(sourceInfo.absoluteFilePath(), stagedPath)) {
        QDir(stagingDir).removeRecursively();
        return failure(QStringLiteral("FILE_READ_FAILED"),
                       QStringLiteral("could not stage upload file as: %1")
                           .arg(stagedPath));
    }

    pending->uploadId = uploadId;
    pending->status = QStringLiteral("queued");
    pending->stagedPath = stagedPath;
    pending->stagingDir = stagingDir;
    pending->contentType = normalizedCt;
    pending->title = sanitizedTitle;
    pending->description = sanitizeDescription(QString());
    pending->tags = normalizeTags(QStringList());
    pending->sizeBytes = sourceInfo.size();
    pending->attemptTimeoutMs = uploadAttemptTimeoutMs(sourceInfo.size());
    pending->deadlineAtMs = QDateTime::currentMSecsSinceEpoch() +
                            uploadRetryBudgetMs(sourceInfo.size());
    return {true, uploadId, QVariant()};
}

void ChroniclePlugin::startStagedUpload(const QString& uploadId) {
    auto it = m_uploads.find(uploadId);
    if (it == m_uploads.end()) {
        return;
    }
    if (!m_activeUploadId.isEmpty() && m_activeUploadId != uploadId) {
        it->status = QStringLiteral("queued");
        return;
    }
    if (it->status == QStringLiteral("uploading")) {
        return;
    }

    if (m_logosAPI == nullptr) {
        it->status = QStringLiteral("error");
        it->errorCode = QStringLiteral("INTERNAL");
        it->error = QStringLiteral("module not initialised - initLogos was not called");
        cleanupUploadFiles(*it);
        return;
    }

    m_activeUploadId = uploadId;
    it->status = QStringLiteral("uploading");
    if (!it->publishId.isEmpty()) {
        auto pub = m_publishes.find(it->publishId);
        if (pub != m_publishes.end() && pub->status == QStringLiteral("queued")) {
            pub->status = QStringLiteral("uploading");
            pub->updatedAtMs = QDateTime::currentMSecsSinceEpoch();
        }
    }
    it->attempt++;
    const int attempt = it->attempt;
    const qint64 attemptStartedAtMs = QDateTime::currentMSecsSinceEpoch();
    const qint64 attemptTimeoutMs = it->attemptTimeoutMs;
    it->sessionId.clear();
    it->errorCode.clear();
    it->error.clear();
    it->nextRetryAtMs = 0;

    TokenManager* tokenManager = m_logosAPI->getTokenManager();

    QTimer::singleShot(static_cast<int>(attemptTimeoutMs), this,
        [this, uploadId, attemptStartedAtMs, attempt]() {
            handleStagedUploadTimeout(uploadId, attemptStartedAtMs, attempt);
        });

    if (tokenManager != nullptr &&
        tokenManager->getToken(QStringLiteral("storage_module")).isEmpty()) {
        tokenManager->saveToken(QStringLiteral("storage_module"),
                                QStringLiteral("chronicle_headless_storage_token_v1"));
    }

    ensureStorageEventSubscription();
    if (m_storageClient == nullptr || !m_storageEventsSubscribed) {
        handleStagedUploadFailure(
            uploadId,
            QStringLiteral("STORAGE_UNAVAILABLE"),
            QStringLiteral("could not subscribe to storage upload events"),
            true);
        return;
    }

    qDebug() << "ChroniclePlugin: uploadUrl attempt"
             << attempt
             << "file"
             << QFileInfo(it->stagedPath).fileName()
             << "size"
             << it->sizeBytes;

    const LogosResult result = parseLogosResult(
        m_storageClient->invokeRemoteMethod(
            QStringLiteral("storage_module"),
            QStringLiteral("uploadUrl"),
            QVariant::fromValue(QUrl::fromLocalFile(it->stagedPath)),
            CHUNK_SIZE,
            Timeout(static_cast<int>(attemptTimeoutMs))));
    if (!result.success) {
        const QString error = result.getError();
        handleStagedUploadFailure(
            uploadId,
            isTransientError(error)
                ? QStringLiteral("STORAGE_UNAVAILABLE")
                : QStringLiteral("STORAGE_REJECTED"),
            error,
            isTransientError(error));
        return;
    }

    const QString sessionId = result.value.toString();
    if (sessionId.isEmpty()) {
        handleStagedUploadFailure(
            uploadId,
            QStringLiteral("STORAGE_REJECTED"),
            QStringLiteral("uploadUrl returned an empty session id"),
            false);
        return;
    }

    it = m_uploads.find(uploadId);
    if (it == m_uploads.end() ||
        it->attempt != attempt ||
        it->status != QStringLiteral("uploading")) {
        return;
    }

    it->sessionId = sessionId;
    m_sessionToUploadId.insert(sessionId, uploadId);
    qDebug() << "ChroniclePlugin: uploadUrl accepted session" << sessionId;
}

void ChroniclePlugin::handleStagedUploadFailure(const QString& uploadId,
                                                const QString& code,
                                                const QString& error,
                                                bool retryable) {
    auto it = m_uploads.find(uploadId);
    if (it == m_uploads.end()) {
        return;
    }

    if (!it->sessionId.isEmpty()) {
        m_sessionToUploadId.remove(it->sessionId);
        it->sessionId.clear();
    }

    it->lastError = error;

    if (retryable) {
        scheduleStagedUploadRetry(uploadId, error);
        clearActiveUpload(uploadId);
        scheduleNextQueuedUpload();
        return;
    }

    it->status = QStringLiteral("error");
    it->errorCode = code;
    it->error = error;
    cleanupUploadFiles(*it);
    const QString publishId = it->publishId;
    clearActiveUpload(uploadId);
    scheduleNextQueuedUpload();
    if (!publishId.isEmpty()) {
        advancePublishAfterUpload(publishId);
    }
}

void ChroniclePlugin::handleStagedUploadTimeout(const QString& uploadId,
                                                qint64 attemptStartedAtMs,
                                                int attempt) {
    auto it = m_uploads.find(uploadId);
    if (it == m_uploads.end() ||
        it->status != QStringLiteral("uploading") ||
        it->attempt != attempt) {
        return;
    }

    const qint64 elapsedMs =
        QDateTime::currentMSecsSinceEpoch() - attemptStartedAtMs;
    handleStagedUploadFailure(
        uploadId,
        QStringLiteral("STORAGE_UNAVAILABLE"),
        QStringLiteral("storage upload attempt timed out after %1 ms")
            .arg(elapsedMs),
        true);
}

void ChroniclePlugin::scheduleStagedUploadRetry(const QString& uploadId,
                                                const QString& lastError) {
    auto it = m_uploads.find(uploadId);
    if (it == m_uploads.end()) {
        return;
    }

    const auto delay = computeBackoff(it->attempt);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 delayMs = static_cast<qint64>(delay.count());

    if (now + delayMs > it->deadlineAtMs) {
        it->status = QStringLiteral("error");
        it->errorCode = QStringLiteral("RETRIES_EXHAUSTED");
        it->error = QStringLiteral(
            "transient storage failures persisted past retry budget; last error: %1")
            .arg(lastError);
        cleanupUploadFiles(*it);
        const QString publishId = it->publishId;
        clearActiveUpload(uploadId);
        scheduleNextQueuedUpload();
        if (!publishId.isEmpty()) {
            advancePublishAfterUpload(publishId);
        }
        return;
    }

    it->status = QStringLiteral("retrying");
    it->errorCode = QStringLiteral("RETRYING");
    it->error = lastError;
    it->nextRetryAtMs = now + delayMs;

    QTimer::singleShot(static_cast<int>(delayMs), this, [this, uploadId]() {
        startStagedUpload(uploadId);
    });
}

void ChroniclePlugin::clearActiveUpload(const QString& uploadId) {
    if (m_activeUploadId == uploadId) {
        m_activeUploadId.clear();
    }
}

void ChroniclePlugin::scheduleNextQueuedUpload() {
    if (!m_activeUploadId.isEmpty()) {
        return;
    }
    for (auto it = m_uploads.begin(); it != m_uploads.end(); ++it) {
        if (it->status == QStringLiteral("queued")) {
            const QString nextUploadId = it.key();
            QTimer::singleShot(0, this, [this, nextUploadId]() {
                startStagedUpload(nextUploadId);
            });
            return;
        }
    }
}

void ChroniclePlugin::handleStorageUploadDone(const QVariantList& args) {
    QString sessionId;
    QString cid;
    bool ok = true;

    if (args.size() >= 3) {
        if (args[0].typeId() == QMetaType::Bool) {
            ok = args[0].toBool();
            sessionId = args[1].toString();
            cid = args[2].toString();
        } else {
            sessionId = args[0].toString();
            cid = args[1].toString();
        }
    } else if (args.size() >= 2) {
        if (args[0].typeId() == QMetaType::Bool) {
            ok = args[0].toBool();
            cid = args[1].toString();
        } else {
            sessionId = args[0].toString();
            cid = args[1].toString();
        }
    } else if (args.size() == 1) {
        if (args[0].typeId() == QMetaType::Bool) {
            ok = args[0].toBool();
        } else {
            cid = args[0].toString();
        }
    }

    QString uploadId;
    if (!sessionId.isEmpty()) {
        uploadId = m_sessionToUploadId.take(sessionId);
    }
    if (uploadId.isEmpty() && !m_uploads.isEmpty()) {
        for (auto it = m_uploads.begin(); it != m_uploads.end(); ++it) {
            if (it->status == QStringLiteral("uploading")) {
                uploadId = it.key();
                break;
            }
        }
    }
    if (uploadId.isEmpty()) {
        qWarning() << "ChroniclePlugin: storageUploadDone without pending upload" << args;
        return;
    }

    auto it = m_uploads.find(uploadId);
    if (it == m_uploads.end()) {
        return;
    }

    if (!ok || cid.isEmpty()) {
        const QString error = cid.isEmpty()
            ? QStringLiteral("storageUploadDone did not include a CID")
            : cid;
        handleStagedUploadFailure(
            uploadId,
            isTransientError(error)
                ? QStringLiteral("STORAGE_UNAVAILABLE")
                : QStringLiteral("STORAGE_REJECTED"),
            error,
            isTransientError(error));
        return;
    }

    if (!it->sessionId.isEmpty()) {
        m_sessionToUploadId.remove(it->sessionId);
        it->sessionId.clear();
    }
    it->status = QStringLiteral("uploaded");
    it->cid = cid;
    it->timestamp = QDateTime::currentSecsSinceEpoch();
    it->metadataHash = hashMetadata(it->contentType,
                                    it->sizeBytes,
                                    it->title,
                                    it->description,
                                    it->tags);
    it->envelope = buildMetadataEnvelope(it->cid,
                                         it->contentType,
                                         it->sizeBytes,
                                         it->timestamp,
                                         it->title,
                                         it->description,
                                         it->tags,
                                         it->metadataHash);
    if (!envelopeWithinCap(it->envelope)) {
        handleStagedUploadFailure(
            uploadId,
            QStringLiteral("ENVELOPE_TOO_LARGE"),
            QStringLiteral("metadata envelope exceeds %1 byte cap")
                .arg(MAX_ENVELOPE_BYTES),
            false);
        return;
    }
    it->errorCode.clear();
    it->error.clear();
    it->nextRetryAtMs = 0;
    cleanupUploadFiles(*it);
    const QString publishId = it->publishId;
    clearActiveUpload(uploadId);
    scheduleNextQueuedUpload();
    if (!publishId.isEmpty()) {
        advancePublishAfterUpload(publishId);
    }
}

void ChroniclePlugin::cleanupUploadFiles(const PendingUpload& pending) const {
    if (!pending.stagingDir.isEmpty()) {
        QDir(pending.stagingDir).removeRecursively();
    }
}

QString ChroniclePlugin::pendingToJson(const PendingUpload& pending) const {
    QJsonObject out;
    out.insert(QStringLiteral("upload_id"), pending.uploadId);
    out.insert(QStringLiteral("status"), pending.status);
    out.insert(QStringLiteral("ok"), pending.status == QStringLiteral("uploaded"));
    out.insert(QStringLiteral("attempt"), pending.attempt);
    out.insert(QStringLiteral("size_bytes"), pending.sizeBytes);
    out.insert(QStringLiteral("content_type"), pending.contentType);
    out.insert(QStringLiteral("attempt_timeout_ms"), pending.attemptTimeoutMs);
    if (pending.nextRetryAtMs > 0) {
        out.insert(QStringLiteral("next_retry_at_ms"), pending.nextRetryAtMs);
    }
    if (!pending.lastError.isEmpty()) {
        out.insert(QStringLiteral("last_error"), pending.lastError);
    }
    if (!pending.cid.isEmpty()) {
        out.insert(QStringLiteral("cid"), pending.cid);
    }
    if (pending.timestamp > 0) {
        out.insert(QStringLiteral("timestamp"), pending.timestamp);
    }
    if (!pending.title.isEmpty()) {
        out.insert(QStringLiteral("title"), pending.title);
    }
    if (!pending.metadataHash.isEmpty()) {
        out.insert(QStringLiteral("metadata_hash"), pending.metadataHash);
    }
    if (!pending.envelope.isEmpty()) {
        out.insert(QStringLiteral("envelope"), pending.envelope);
    }
    if (!pending.errorCode.isEmpty()) {
        out.insert(QStringLiteral("code"), pending.errorCode);
        out.insert(QStringLiteral("error"), pending.error);
    }
    return QString::fromUtf8(
        QJsonDocument(out).toJson(QJsonDocument::Compact));
}

QString ChroniclePlugin::broadcastToJson(const PendingBroadcast& pending) const {
    QJsonObject out;
    out.insert(QStringLiteral("broadcast_id"), pending.broadcastId);
    out.insert(QStringLiteral("status"), pending.status);
    out.insert(QStringLiteral("ok"),
               pending.status == QStringLiteral("sent") ||
               pending.status == QStringLiteral("deduped"));
    out.insert(QStringLiteral("topic"), pending.topic);
    out.insert(QStringLiteral("cid"), pending.cid);
    out.insert(QStringLiteral("metadata_hash"), pending.metadataHash);
    out.insert(QStringLiteral("deduped"), pending.deduped);
    out.insert(QStringLiteral("created_at_ms"), pending.createdAtMs);
    out.insert(QStringLiteral("updated_at_ms"), pending.updatedAtMs);
    if (!pending.envelope.isEmpty()) {
        out.insert(QStringLiteral("envelope"), pending.envelope);
    }
    if (!pending.errorCode.isEmpty()) {
        out.insert(QStringLiteral("code"), pending.errorCode);
        out.insert(QStringLiteral("error"), pending.error);
    }
    return QString::fromUtf8(
        QJsonDocument(out).toJson(QJsonDocument::Compact));
}

QString ChroniclePlugin::errorToJson(const QString& code,
                                     const QString& error) const {
    QJsonObject out;
    out.insert(QStringLiteral("queued"), false);
    out.insert(QStringLiteral("ok"), false);
    out.insert(QStringLiteral("code"), code);
    out.insert(QStringLiteral("error"), error);
    return QString::fromUtf8(
        QJsonDocument(out).toJson(QJsonDocument::Compact));
}

// ---------------------------------------------------------------------------
// Publish methods
// ---------------------------------------------------------------------------

QString ChroniclePlugin::publishFileJson(const QString& requestJson) {
    QJsonParseError jsonError;
    const QJsonDocument doc =
        QJsonDocument::fromJson(requestJson.toUtf8(), &jsonError);
    if (jsonError.error != QJsonParseError::NoError || !doc.isObject()) {
        return errorToJson(QStringLiteral("INVALID_REQUEST"),
                           QStringLiteral("requestJson must be a JSON object"));
    }

    const QJsonObject req = doc.object();

    const QString path = req.value(QStringLiteral("path")).toString().trimmed();
    if (path.isEmpty()) {
        return errorToJson(QStringLiteral("MISSING_FIELD"),
                           QStringLiteral("path is required"));
    }
    const QString contentType =
        req.value(QStringLiteral("content_type")).toString().trimmed();
    if (contentType.isEmpty()) {
        return errorToJson(QStringLiteral("MISSING_FIELD"),
                           QStringLiteral("content_type is required"));
    }
    const QString title =
        req.value(QStringLiteral("title")).toString().trimmed();
    if (title.isEmpty()) {
        return errorToJson(QStringLiteral("MISSING_FIELD"),
                           QStringLiteral("title is required"));
    }

    const QString description =
        req.value(QStringLiteral("description")).toString();
    QStringList tags;
    QString tagsError;
    if (!parseTagsValue(req, &tags, &tagsError)) {
        return errorToJson(QStringLiteral("INVALID_TAGS"), tagsError);
    }
    const bool broadcastRequested =
        req.value(QStringLiteral("broadcast")).toBool(true);

    PendingUpload upload;
    const LogosResult prepared =
        prepareStagedUpload(path, contentType, title, &upload);
    if (!prepared.success) {
        return errorToJson(prepared.value.toString(),
                           prepared.error.toString());
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const QString publishId =
        QUuid::createUuid().toString(QUuid::WithoutBraces);

    upload.description = sanitizeDescription(description);
    upload.tags = normalizeTags(tags);
    upload.publishId = publishId;
    m_uploads.insert(upload.uploadId, upload);

    PendingPublish publish;
    publish.publishId = publishId;
    publish.uploadId = upload.uploadId;
    publish.status = QStringLiteral("queued");
    publish.contentType = upload.contentType;
    publish.title = upload.title;
    publish.description = upload.description;
    publish.tags = upload.tags;
    publish.broadcastRequested = broadcastRequested;
    publish.createdAtMs = now;
    publish.updatedAtMs = now;
    m_publishes.insert(publishId, publish);
    persistPublishRecord(publish);

    if (m_activeUploadId.isEmpty()) {
        QTimer::singleShot(ASYNC_UPLOAD_START_DELAY_MS, this,
                           [this, uploadId = upload.uploadId]() {
                               startStagedUpload(uploadId);
                           });
    }

    QJsonObject out;
    out.insert(QStringLiteral("queued"), true);
    out.insert(QStringLiteral("ok"), false);
    out.insert(QStringLiteral("publish_id"), publishId);
    out.insert(QStringLiteral("upload_id"), upload.uploadId);
    out.insert(QStringLiteral("status"), QStringLiteral("queued"));
    return compactJson(out);
}

QString ChroniclePlugin::publishStatusJson(const QString& publishId) {
    const auto it = m_publishes.constFind(publishId);
    if (it == m_publishes.constEnd()) {
        return errorToJson(
            QStringLiteral("UNKNOWN_PUBLISH"),
            QStringLiteral("unknown publish_id: %1").arg(publishId));
    }
    return publishToJson(it.value());
}

QString ChroniclePlugin::listPublishedJson() {
    QList<PendingPublish> sorted;
    sorted.reserve(m_publishes.size());
    for (const PendingPublish& pub : std::as_const(m_publishes)) {
        sorted.append(pub);
    }
    std::sort(sorted.begin(), sorted.end(),
              [](const PendingPublish& a, const PendingPublish& b) {
                  return a.createdAtMs > b.createdAtMs;
              });

    QJsonArray arr;
    for (const PendingPublish& pub : sorted) {
        arr.append(
            QJsonDocument::fromJson(publishToJson(pub).toUtf8()).object());
    }

    QJsonObject out;
    out.insert(QStringLiteral("ok"), true);
    out.insert(QStringLiteral("records"), arr);
    return compactJson(out);
}

QString ChroniclePlugin::clearPublishedJson() {
    // Refuse if any publish is still in flight. A clear mid-stream would
    // orphan a queued upload or pending broadcast — and when the upload
    // completes its callback would write a fresh record back into the
    // freshly-empty ledger, partly defeating the clear.
    QStringList inFlight;
    for (auto it = m_publishes.constBegin();
         it != m_publishes.constEnd(); ++it) {
        const QString& s = it->status;
        if (s != QStringLiteral("broadcast_sent") &&
            s != QStringLiteral("error")) {
            inFlight.append(it->publishId);
        }
    }
    if (!inFlight.isEmpty()) {
        return errorToJson(
            QStringLiteral("PUBLISH_IN_FLIGHT"),
            QStringLiteral("cannot clear: %1 publish(es) still in progress")
                .arg(inFlight.size()));
    }

    const int cleared = m_publishes.size();
    m_publishes.clear();
    m_publishDedupe.clear();

    const QString path = publishLedgerPath();
    if (QFile::exists(path) && !QFile::remove(path)) {
        qWarning() << "ChroniclePlugin: failed to delete publish ledger:"
                   << path;
    }

    qDebug() << "ChroniclePlugin: cleared" << cleared << "publish records";

    QJsonObject out;
    out.insert(QStringLiteral("ok"), true);
    out.insert(QStringLiteral("cleared"), cleared);
    return compactJson(out);
}

// ---------------------------------------------------------------------------
// Anchor API (phase 1 — config is functional, action methods are stubs)
// ---------------------------------------------------------------------------

QString ChroniclePlugin::anchorCapabilitiesJson() {
    if (!m_anchorConfigLoaded) {
        m_anchorConfig = AnchorConfigStore::load();
        m_anchorConfigLoaded = true;
    }
    const QStringList missing = m_anchorConfig.missingFields();
    const bool configured = missing.isEmpty();

    QJsonArray missingArr;
    for (const QString& f : missing) missingArr.append(f);

    QJsonObject out;
    out.insert(QStringLiteral("ok"), true);
    out.insert(QStringLiteral("enabled"), configured);
    out.insert(QStringLiteral("configured"), configured);
    out.insert(QStringLiteral("missing_fields"), missingArr);
    out.insert(QStringLiteral("program_id"), m_anchorConfig.programId);
    out.insert(QStringLiteral("backend"), QStringLiteral("stub"));
    return compactJson(out);
}

QString ChroniclePlugin::getAnchorConfigJson() {
    if (!m_anchorConfigLoaded) {
        m_anchorConfig = AnchorConfigStore::load();
        m_anchorConfigLoaded = true;
    }
    QJsonObject out;
    out.insert(QStringLiteral("ok"), true);
    out.insert(QStringLiteral("config"), m_anchorConfig.toJson());
    return compactJson(out);
}

QString ChroniclePlugin::setAnchorConfigJson(const QString& cfgJson) {
    const QJsonDocument doc = QJsonDocument::fromJson(cfgJson.toUtf8());
    if (!doc.isObject()) {
        return errorToJson(QStringLiteral("BAD_CONFIG"),
                           QStringLiteral("request body must be a JSON object"));
    }
    AnchorConfig cfg = AnchorConfig::fromJson(doc.object());

    QString saveErr;
    if (!AnchorConfigStore::save(cfg, &saveErr)) {
        return errorToJson(QStringLiteral("CONFIG_WRITE_FAILED"),
                           saveErr.isEmpty() ? QStringLiteral("could not write anchor config")
                                             : saveErr);
    }
    m_anchorConfig = cfg;
    m_anchorConfigLoaded = true;

    const QStringList missing = cfg.missingFields();
    QJsonArray missingArr;
    for (const QString& f : missing) missingArr.append(f);

    QJsonObject out;
    out.insert(QStringLiteral("ok"), true);
    out.insert(QStringLiteral("configured"), missing.isEmpty());
    out.insert(QStringLiteral("missing_fields"), missingArr);
    return compactJson(out);
}

QString ChroniclePlugin::anchorBatchJson(const QString& requestJson) {
    if (!m_anchorConfigLoaded) {
        m_anchorConfig = AnchorConfigStore::load();
        m_anchorConfigLoaded = true;
    }
    if (!m_anchorsLoaded) loadAnchorLedger();

    // Don't even load the FFI for unconfigured attempts.
    const QStringList missing = m_anchorConfig.missingFields();
    if (!missing.isEmpty()) {
        QJsonArray arr;
        for (const QString& f : missing) arr.append(f);
        QJsonObject out;
        out.insert(QStringLiteral("ok"), false);
        out.insert(QStringLiteral("code"), QStringLiteral("ANCHOR_NOT_CONFIGURED"));
        out.insert(QStringLiteral("error"),
                   QStringLiteral("anchor settings incomplete; populate via setAnchorConfigJson"));
        out.insert(QStringLiteral("missing_fields"), arr);
        return compactJson(out);
    }

    const QJsonDocument reqDoc = QJsonDocument::fromJson(requestJson.toUtf8());
    const QJsonArray entries = reqDoc.object()
                                     .value(QStringLiteral("entries")).toArray();
    if (entries.isEmpty()) {
        return errorToJson(QStringLiteral("BAD_REQUEST"),
                           QStringLiteral("entries array empty or missing"));
    }

    if (m_anchorClient == nullptr) {
        m_anchorClient = new ChronicleAnchorClient(this);
    }

    // Compose the FFI request. Chronicle stores metadata_hash with a `v1:`
    // version prefix; the FFI hex-decodes the field directly, so strip it.
    QJsonArray ffiEntries;
    for (const QJsonValue& v : entries) {
        QJsonObject e = v.toObject();
        QString h = e.value(QStringLiteral("metadata_hash")).toString();
        if (h.startsWith(QStringLiteral("v1:"))) h.remove(0, 3);
        e.insert(QStringLiteral("metadata_hash"), h);
        ffiEntries.append(e);
    }

    QJsonObject ffiArgs;
    ffiArgs.insert(QStringLiteral("program_id_hex"),  m_anchorConfig.programId);
    ffiArgs.insert(QStringLiteral("wallet_path"),     m_anchorConfig.walletHome);
    ffiArgs.insert(QStringLiteral("sequencer_url"),   m_anchorConfig.sequencerUrl);
    ffiArgs.insert(QStringLiteral("anchorer"),        m_anchorConfig.signerAccountId);
    ffiArgs.insert(QStringLiteral("entries"),         ffiEntries);
    const QString ffiArgsJson = QString::fromUtf8(
        QJsonDocument(ffiArgs).toJson(QJsonDocument::Compact));

    // Synchronous — the FFI's tokio runtime handles its own internal async.
    // Single CID per UI click takes ~1-3s against a local sequencer.
    const QString respJson = m_anchorClient->indexBatch(ffiArgsJson);
    const QJsonObject resp = QJsonDocument::fromJson(respJson.toUtf8()).object();

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const bool success = resp.value(QStringLiteral("ok")).toBool();
    const QString txHash = resp.value(QStringLiteral("tx_hash")).toString();
    const QString errorStr = resp.value(QStringLiteral("error")).toString();

    // Persist a terminal record per CID — confirmed on success, failed otherwise.
    for (const QJsonValue& v : entries) {
        const QJsonObject e = v.toObject();
        AnchorRecord ar;
        ar.publishId     = e.value(QStringLiteral("publish_id")).toString();
        ar.cid           = e.value(QStringLiteral("cid")).toString();
        ar.metadataHash  = e.value(QStringLiteral("metadata_hash")).toString();
        ar.attemptedAtMs = now;
        if (success) {
            ar.state         = QStringLiteral("confirmed");
            ar.txHash        = txHash;
            ar.confirmedAtMs = now;
        } else {
            ar.state = QStringLiteral("failed");
            ar.code  = QStringLiteral("ANCHOR_FAILED");
            ar.error = errorStr;
        }
        if (!ar.cid.isEmpty()) {
            m_anchors.insert(ar.cid, ar);
            persistAnchorRecord(ar);
        }
    }

    // Pass the FFI's response shape through; UI keys on `ok` and the
    // refreshed anchor map for everything else.
    return respJson;
}

QString ChroniclePlugin::initRegistryJson() {
    if (!m_anchorConfigLoaded) {
        m_anchorConfig = AnchorConfigStore::load();
        m_anchorConfigLoaded = true;
    }
    const QStringList missing = m_anchorConfig.missingFields();
    if (!missing.isEmpty()) {
        QJsonArray arr;
        for (const QString& f : missing) arr.append(f);
        QJsonObject out;
        out.insert(QStringLiteral("ok"), false);
        out.insert(QStringLiteral("code"), QStringLiteral("ANCHOR_NOT_CONFIGURED"));
        out.insert(QStringLiteral("error"),
                   QStringLiteral("anchor settings incomplete; populate via setAnchorConfigJson"));
        out.insert(QStringLiteral("missing_fields"), arr);
        return compactJson(out);
    }
    if (m_anchorClient == nullptr) m_anchorClient = new ChronicleAnchorClient(this);

    QJsonObject ffiArgs;
    ffiArgs.insert(QStringLiteral("program_id_hex"), m_anchorConfig.programId);
    ffiArgs.insert(QStringLiteral("wallet_path"),    m_anchorConfig.walletHome);
    ffiArgs.insert(QStringLiteral("sequencer_url"),  m_anchorConfig.sequencerUrl);
    ffiArgs.insert(QStringLiteral("anchorer"),       m_anchorConfig.signerAccountId);
    return m_anchorClient->initRegistry(
        QString::fromUtf8(QJsonDocument(ffiArgs).toJson(QJsonDocument::Compact)));
}

QString ChroniclePlugin::getRegistryJson() {
    if (!m_anchorConfigLoaded) {
        m_anchorConfig = AnchorConfigStore::load();
        m_anchorConfigLoaded = true;
    }
    const QStringList missing = m_anchorConfig.missingFields();
    if (!missing.isEmpty()) {
        QJsonArray arr;
        for (const QString& f : missing) arr.append(f);
        QJsonObject out;
        out.insert(QStringLiteral("ok"), false);
        out.insert(QStringLiteral("code"), QStringLiteral("ANCHOR_NOT_CONFIGURED"));
        out.insert(QStringLiteral("error"),
                   QStringLiteral("anchor settings incomplete; populate via setAnchorConfigJson"));
        out.insert(QStringLiteral("missing_fields"), arr);
        return compactJson(out);
    }
    if (m_anchorClient == nullptr) m_anchorClient = new ChronicleAnchorClient(this);

    QJsonObject ffiArgs;
    ffiArgs.insert(QStringLiteral("program_id_hex"), m_anchorConfig.programId);
    ffiArgs.insert(QStringLiteral("wallet_path"),    m_anchorConfig.walletHome);
    ffiArgs.insert(QStringLiteral("sequencer_url"),  m_anchorConfig.sequencerUrl);
    return m_anchorClient->getRegistry(
        QString::fromUtf8(QJsonDocument(ffiArgs).toJson(QJsonDocument::Compact)));
}

QString ChroniclePlugin::setBroadcastTopic(const QString& topic) {
    m_broadcastTopic = topic.isEmpty()
        ? QString::fromLatin1(CHRONICLE_TOPIC)
        : topic;
    QJsonObject out;
    out.insert(QStringLiteral("ok"), true);
    out.insert(QStringLiteral("topic"), m_broadcastTopic);
    return compactJson(out);
}

QString ChroniclePlugin::getBroadcastTopic() {
    QJsonObject out;
    out.insert(QStringLiteral("ok"), true);
    out.insert(QStringLiteral("topic"), m_broadcastTopic);
    return compactJson(out);
}

QString ChroniclePlugin::anchorStatusJson(const QString& anchorId) {
    // Phase 1 never hands out an anchor_id — anchorBatchJson is synchronous,
    // its result is the terminal state. Phase 2 will populate an in-flight
    // map and respond from it here.
    QJsonObject out;
    out.insert(QStringLiteral("ok"), false);
    out.insert(QStringLiteral("code"), QStringLiteral("UNKNOWN_ANCHOR_ID"));
    out.insert(QStringLiteral("error"),
               QStringLiteral("unknown anchor id: %1").arg(anchorId));
    return compactJson(out);
}

QString ChroniclePlugin::lookupAnchorJson(const QString& cid) {
    // Reads from the persisted ledger — no chain round-trip. The chronicle
    // registry program is idempotent so the local cache is treated as
    // authoritative within a session; reconciliation with chain is a
    // separate explicit operation we'll add later if needed.
    if (!m_anchorsLoaded) loadAnchorLedger();
    auto it = m_anchors.constFind(cid);
    QJsonObject out;
    out.insert(QStringLiteral("ok"), true);
    if (it == m_anchors.constEnd()) {
        out.insert(QStringLiteral("found"), false);
        out.insert(QStringLiteral("cid"), cid);
        return compactJson(out);
    }
    out.insert(QStringLiteral("found"), true);
    out.insert(QStringLiteral("record"), anchorRecordToJson(*it));
    return compactJson(out);
}

QString ChroniclePlugin::listAnchorsJson() {
    if (!m_anchorsLoaded) loadAnchorLedger();
    QJsonObject anchors;
    for (auto it = m_anchors.constBegin(); it != m_anchors.constEnd(); ++it) {
        anchors.insert(it.key(), anchorRecordToJson(it.value()));
    }
    QJsonObject out;
    out.insert(QStringLiteral("ok"), true);
    out.insert(QStringLiteral("anchors"), anchors);
    return compactJson(out);
}

QString ChroniclePlugin::clearAnchorsJson() {
    if (!m_anchorsLoaded) loadAnchorLedger();
    const int cleared = m_anchors.size();
    m_anchors.clear();
    const QString path = anchorLedgerPath();
    if (QFile::exists(path) && !QFile::remove(path)) {
        qWarning() << "ChroniclePlugin: failed to delete anchor ledger:" << path;
    }
    QJsonObject out;
    out.insert(QStringLiteral("ok"), true);
    out.insert(QStringLiteral("cleared"), cleared);
    return compactJson(out);
}

QString ChroniclePlugin::anchorLedgerPath() const {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
           QStringLiteral("/chronicle/anchor-ledger.jsonl");
}

QJsonObject ChroniclePlugin::anchorRecordToJson(const AnchorRecord& record) const {
    QJsonObject obj;
    obj.insert(QStringLiteral("publish_id"),       record.publishId);
    obj.insert(QStringLiteral("cid"),              record.cid);
    obj.insert(QStringLiteral("metadata_hash"),    record.metadataHash);
    obj.insert(QStringLiteral("state"),            record.state);
    obj.insert(QStringLiteral("tx_hash"),          record.txHash);
    obj.insert(QStringLiteral("code"),             record.code);
    obj.insert(QStringLiteral("error"),            record.error);
    obj.insert(QStringLiteral("attempted_at_ms"),  record.attemptedAtMs);
    obj.insert(QStringLiteral("confirmed_at_ms"),  record.confirmedAtMs);
    return obj;
}

void ChroniclePlugin::persistAnchorRecord(const AnchorRecord& record) {
    const QString path = anchorLedgerPath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::Append | QIODevice::Text)) {
        qWarning() << "ChroniclePlugin: could not write anchor ledger:" << path;
        return;
    }
    QJsonObject entry;
    entry.insert(QStringLiteral("type"), QStringLiteral("anchor_updated"));
    entry.insert(QStringLiteral("record"), anchorRecordToJson(record));
    QTextStream out(&f);
    out << QJsonDocument(entry).toJson(QJsonDocument::Compact) << QChar('\n');
}

void ChroniclePlugin::loadAnchorLedger() {
    m_anchorsLoaded = true;
    const QString path = anchorLedgerPath();
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return;
    }
    QHash<QString, QJsonObject> latest;
    QTextStream in(&f);
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (line.isEmpty()) continue;
        const QJsonDocument doc = QJsonDocument::fromJson(line.toUtf8());
        if (!doc.isObject()) continue;
        const QJsonObject entry = doc.object();
        if (entry.value(QStringLiteral("type")).toString() !=
            QStringLiteral("anchor_updated")) continue;
        const QJsonObject rec = entry.value(QStringLiteral("record")).toObject();
        const QString cid = rec.value(QStringLiteral("cid")).toString();
        if (!cid.isEmpty()) latest.insert(cid, rec);
    }
    for (auto it = latest.constBegin(); it != latest.constEnd(); ++it) {
        const QJsonObject& rec = it.value();
        AnchorRecord ar;
        ar.publishId      = rec.value(QStringLiteral("publish_id")).toString();
        ar.cid            = rec.value(QStringLiteral("cid")).toString();
        ar.metadataHash   = rec.value(QStringLiteral("metadata_hash")).toString();
        ar.state          = rec.value(QStringLiteral("state")).toString();
        ar.txHash         = rec.value(QStringLiteral("tx_hash")).toString();
        ar.code           = rec.value(QStringLiteral("code")).toString();
        ar.error          = rec.value(QStringLiteral("error")).toString();
        ar.attemptedAtMs  = rec.value(QStringLiteral("attempted_at_ms")).toVariant().toLongLong();
        ar.confirmedAtMs  = rec.value(QStringLiteral("confirmed_at_ms")).toVariant().toLongLong();
        m_anchors.insert(ar.cid, ar);
    }
    qDebug() << "ChroniclePlugin: loaded" << m_anchors.size()
             << "anchor records from ledger";
}

// ---------------------------------------------------------------------------
// Publish private helpers
// ---------------------------------------------------------------------------

void ChroniclePlugin::advancePublishAfterUpload(const QString& publishId) {
    auto pub = m_publishes.find(publishId);
    if (pub == m_publishes.end()) {
        return;
    }

    const auto up = m_uploads.constFind(pub->uploadId);
    if (up == m_uploads.constEnd()) {
        return;
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    if (up->status == QStringLiteral("error")) {
        pub->status = QStringLiteral("error");
        pub->errorCode = up->errorCode;
        pub->error = up->error;
        pub->updatedAtMs = now;
        persistPublishRecord(*pub);
        return;
    }

    if (up->status != QStringLiteral("uploaded")) {
        return;
    }

    pub->cid = up->cid;
    pub->sizeBytes = up->sizeBytes;
    pub->contentType = up->contentType;
    pub->metadataHash = up->metadataHash;
    pub->envelope = up->envelope;
    pub->status = QStringLiteral("uploaded");
    pub->updatedAtMs = now;
    persistPublishRecord(*pub);

    pub->status = QStringLiteral("envelope_built");
    pub->updatedAtMs = now;
    persistPublishRecord(*pub);

    const QString dedupeKey =
        pub->cid + QStringLiteral(":") + pub->metadataHash;
    if (m_publishDedupe.contains(dedupeKey)) {
        pub->originalPublishId = m_publishDedupe.value(dedupeKey);
        pub->status = QStringLiteral("error");
        pub->errorCode = QStringLiteral("DUPLICATE");
        pub->error = QStringLiteral(
            "document already published; see original_publish_id");
        pub->updatedAtMs = now;
        persistPublishRecord(*pub);
        return;
    }

    if (!pub->broadcastRequested) {
        m_publishDedupe.insert(dedupeKey, publishId);
        pub->status = QStringLiteral("broadcast_sent");
        pub->updatedAtMs = now;
        persistPublishRecord(*pub);
        return;
    }

    PendingBroadcast broadcast;
    broadcast.broadcastId =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    broadcast.status = QStringLiteral("queued");
    broadcast.topic = m_broadcastTopic;
    broadcast.cid = pub->cid;
    broadcast.metadataHash = pub->metadataHash;
    broadcast.dedupeKey = dedupeKey;
    broadcast.envelope = pub->envelope;
    broadcast.createdAtMs = now;
    broadcast.updatedAtMs = now;

    m_broadcasts.insert(broadcast.broadcastId, broadcast);
    m_broadcastDedupe.insert(dedupeKey);
    m_broadcastToPublishId.insert(broadcast.broadcastId, publishId);

    pub->broadcastId = broadcast.broadcastId;
    pub->status = QStringLiteral("broadcasting");
    pub->updatedAtMs = now;
    persistPublishRecord(*pub);

    QTimer::singleShot(0, this,
                       [this, broadcastId = broadcast.broadcastId]() {
                           startBroadcastSend(broadcastId);
                       });
}

void ChroniclePlugin::advancePublishAfterBroadcast(
    const QString& broadcastId) {
    const QString publishId = m_broadcastToPublishId.take(broadcastId);
    if (publishId.isEmpty()) {
        return;
    }

    auto pub = m_publishes.find(publishId);
    if (pub == m_publishes.end()) {
        return;
    }

    const auto bc = m_broadcasts.constFind(broadcastId);
    if (bc == m_broadcasts.constEnd()) {
        return;
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    if (bc->status == QStringLiteral("sent")) {
        const QString dedupeKey =
            pub->cid + QStringLiteral(":") + pub->metadataHash;
        m_publishDedupe.insert(dedupeKey, publishId);
        pub->status = QStringLiteral("broadcast_sent");
        pub->errorCode.clear();
        pub->error.clear();
    } else {
        pub->status = QStringLiteral("error");
        pub->errorCode = bc->errorCode.isEmpty()
            ? QStringLiteral("BROADCAST_FAILED")
            : bc->errorCode;
        pub->error = bc->error.isEmpty()
            ? QStringLiteral("broadcast failed")
            : bc->error;
    }
    pub->updatedAtMs = now;
    persistPublishRecord(*pub);
}

QString ChroniclePlugin::publishLedgerPath() const {
    return QStandardPaths::writableLocation(
               QStandardPaths::AppDataLocation) +
           QStringLiteral("/chronicle/publish-ledger.jsonl");
}

void ChroniclePlugin::persistPublishRecord(const PendingPublish& publish) {
    const QString path = publishLedgerPath();
    const QFileInfo info(path);
    QDir dir = info.dir();
    if (!dir.exists()) {
        dir.mkpath(QStringLiteral("."));
    }

    QFile file(path);
    if (!file.open(QIODevice::Append | QIODevice::Text)) {
        qWarning() << "ChroniclePlugin: could not write publish ledger:"
                   << path;
        return;
    }

    QJsonObject record;
    record.insert(QStringLiteral("publish_id"), publish.publishId);
    record.insert(QStringLiteral("upload_id"), publish.uploadId);
    record.insert(QStringLiteral("broadcast_id"), publish.broadcastId);
    if (!publish.originalPublishId.isEmpty()) {
        record.insert(QStringLiteral("original_publish_id"),
                      publish.originalPublishId);
    }
    record.insert(QStringLiteral("status"), publish.status);
    record.insert(QStringLiteral("error_code"), publish.errorCode);
    record.insert(QStringLiteral("error"), publish.error);
    record.insert(QStringLiteral("content_type"), publish.contentType);
    record.insert(QStringLiteral("title"), publish.title);
    record.insert(QStringLiteral("description"), publish.description);
    QJsonArray tagsArr;
    for (const QString& tag : publish.tags) {
        tagsArr.append(tag);
    }
    record.insert(QStringLiteral("tags"), tagsArr);
    record.insert(QStringLiteral("cid"), publish.cid);
    record.insert(QStringLiteral("size_bytes"), publish.sizeBytes);
    record.insert(QStringLiteral("metadata_hash"), publish.metadataHash);
    if (!publish.envelope.isEmpty()) {
        record.insert(QStringLiteral("envelope"), publish.envelope);
    }
    record.insert(QStringLiteral("created_at_ms"), publish.createdAtMs);
    record.insert(QStringLiteral("updated_at_ms"), publish.updatedAtMs);
    record.insert(QStringLiteral("broadcast_requested"),
                  publish.broadcastRequested);

    QJsonObject entry;
    entry.insert(QStringLiteral("type"), QStringLiteral("publish_updated"));
    entry.insert(QStringLiteral("record"), record);

    QTextStream out(&file);
    out << QJsonDocument(entry).toJson(QJsonDocument::Compact) << QChar('\n');
}

void ChroniclePlugin::loadPublishLedger() {
    const QString path = publishLedgerPath();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return;
    }

    QHash<QString, QJsonObject> latest;
    QTextStream in(&file);
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (line.isEmpty()) {
            continue;
        }
        const QJsonDocument doc =
            QJsonDocument::fromJson(line.toUtf8());
        if (!doc.isObject()) {
            continue;
        }
        const QJsonObject entry = doc.object();
        if (entry.value(QStringLiteral("type")).toString() !=
            QStringLiteral("publish_updated")) {
            continue;
        }
        const QJsonObject record =
            entry.value(QStringLiteral("record")).toObject();
        const QString publishId =
            record.value(QStringLiteral("publish_id")).toString();
        if (!publishId.isEmpty()) {
            latest.insert(publishId, record);
        }
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (const QJsonObject& record : std::as_const(latest)) {
        PendingPublish pub;
        pub.publishId =
            record.value(QStringLiteral("publish_id")).toString();
        pub.uploadId =
            record.value(QStringLiteral("upload_id")).toString();
        pub.broadcastId =
            record.value(QStringLiteral("broadcast_id")).toString();
        pub.originalPublishId =
            record.value(QStringLiteral("original_publish_id")).toString();
        pub.status = record.value(QStringLiteral("status")).toString();
        pub.errorCode =
            record.value(QStringLiteral("error_code")).toString();
        pub.error = record.value(QStringLiteral("error")).toString();
        pub.contentType =
            record.value(QStringLiteral("content_type")).toString();
        pub.title = record.value(QStringLiteral("title")).toString();
        pub.description =
            record.value(QStringLiteral("description")).toString();
        for (const QJsonValue& v :
             record.value(QStringLiteral("tags")).toArray()) {
            pub.tags.append(v.toString());
        }
        pub.cid = record.value(QStringLiteral("cid")).toString();
        pub.sizeBytes =
            record.value(QStringLiteral("size_bytes")).toInteger();
        pub.metadataHash =
            record.value(QStringLiteral("metadata_hash")).toString();
        pub.envelope =
            record.value(QStringLiteral("envelope")).toObject();
        pub.createdAtMs =
            record.value(QStringLiteral("created_at_ms")).toInteger();
        pub.updatedAtMs =
            record.value(QStringLiteral("updated_at_ms")).toInteger();
        pub.broadcastRequested =
            record.value(QStringLiteral("broadcast_requested")).toBool(true);

        const bool isTerminal =
            pub.status == QStringLiteral("broadcast_sent") ||
            pub.status == QStringLiteral("error");
        if (!isTerminal) {
            pub.status = QStringLiteral("error");
            pub.errorCode = QStringLiteral("interrupted");
            pub.error =
                QStringLiteral("Publish was interrupted before completion");
            pub.updatedAtMs = now;
        }

        if (pub.status == QStringLiteral("broadcast_sent") &&
            !pub.cid.isEmpty() && !pub.metadataHash.isEmpty()) {
            m_publishDedupe.insert(
                pub.cid + QStringLiteral(":") + pub.metadataHash,
                pub.publishId);
        }

        m_publishes.insert(pub.publishId, pub);
    }

    qDebug() << "ChroniclePlugin: loaded" << m_publishes.size()
             << "publish records from ledger";
}

QString ChroniclePlugin::publishToJson(const PendingPublish& publish) const {
    QJsonObject out;
    out.insert(QStringLiteral("publish_id"), publish.publishId);
    out.insert(QStringLiteral("upload_id"), publish.uploadId);
    if (!publish.broadcastId.isEmpty()) {
        out.insert(QStringLiteral("broadcast_id"), publish.broadcastId);
    }
    if (!publish.originalPublishId.isEmpty()) {
        out.insert(QStringLiteral("original_publish_id"),
                   publish.originalPublishId);
    }
    out.insert(QStringLiteral("status"), publish.status);
    out.insert(QStringLiteral("ok"),
               publish.status == QStringLiteral("broadcast_sent"));
    out.insert(QStringLiteral("content_type"), publish.contentType);
    out.insert(QStringLiteral("title"), publish.title);
    out.insert(QStringLiteral("description"), publish.description);
    QJsonArray tagsArr;
    for (const QString& tag : publish.tags) {
        tagsArr.append(tag);
    }
    out.insert(QStringLiteral("tags"), tagsArr);
    out.insert(QStringLiteral("size_bytes"), publish.sizeBytes);
    if (!publish.cid.isEmpty()) {
        out.insert(QStringLiteral("cid"), publish.cid);
    }
    if (!publish.metadataHash.isEmpty()) {
        out.insert(QStringLiteral("metadata_hash"), publish.metadataHash);
    }
    if (!publish.envelope.isEmpty()) {
        out.insert(QStringLiteral("envelope"), publish.envelope);
    }
    out.insert(QStringLiteral("created_at_ms"), publish.createdAtMs);
    out.insert(QStringLiteral("updated_at_ms"), publish.updatedAtMs);
    if (!publish.errorCode.isEmpty()) {
        out.insert(QStringLiteral("code"), publish.errorCode);
        out.insert(QStringLiteral("error"), publish.error);
    }
    return compactJson(out);
}
