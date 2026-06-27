#ifndef CHRONICLE_PLUGIN_H
#define CHRONICLE_PLUGIN_H

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantList>

#include "chronicle_anchor_client.h"
#include "chronicle_anchor_config.h"
#include "chronicle_interface.h"
#include "logos_api.h"

class LogosAPIClient;
class LogosObject;

class ChroniclePlugin : public QObject, public ChronicleInterface
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID ChronicleInterface_iid FILE "metadata.json")
    Q_INTERFACES(ChronicleInterface PluginInterface)

public:
    ChroniclePlugin();
    ~ChroniclePlugin() override;

    QString name() const override { return "chronicle"; }
    QString version() const override { return "1.0.0"; }

    Q_INVOKABLE void initLogos(LogosAPI* api);

    Q_INVOKABLE QString health() override;
    Q_INVOKABLE QString uploadFileJson(const QString& path,
                                       const QString& contentType,
                                       const QString& title) override;
    Q_INVOKABLE QString uploadStatusJson(const QString& uploadId) override;
    Q_INVOKABLE QString normalizeContentTypeJson(
        const QString& contentType) override;
    Q_INVOKABLE QString hashMetadataJson(const QString& contentType,
                                         const QString& sizeBytes,
                                         const QString& title,
                                         const QString& description,
                                         const QString& tagsJson) override;
    Q_INVOKABLE QString buildMetadataEnvelopeJson(
        const QString& envelopeInputJson) override;
    Q_INVOKABLE QString startBroadcasterJson() override;
    Q_INVOKABLE QString broadcastEnvelopeJson(
        const QString& envelopeJson) override;
    Q_INVOKABLE QString broadcastStatusJson(
        const QString& broadcastId) override;
    Q_INVOKABLE QString publishFileJson(
        const QString& requestJson) override;
    Q_INVOKABLE QString publishStatusJson(
        const QString& publishId) override;
    Q_INVOKABLE QString listPublishedJson() override;
    Q_INVOKABLE QString clearPublishedJson() override;

    Q_INVOKABLE QString anchorCapabilitiesJson() override;
    Q_INVOKABLE QString getAnchorConfigJson() override;
    Q_INVOKABLE QString setAnchorConfigJson(const QString& cfgJson) override;
    Q_INVOKABLE QString anchorBatchJson(const QString& requestJson) override;
    Q_INVOKABLE QString anchorStatusJson(const QString& anchorId) override;
    Q_INVOKABLE QString lookupAnchorJson(const QString& cid) override;
    Q_INVOKABLE QString listAnchorsJson() override;
    Q_INVOKABLE QString clearAnchorsJson() override;

    Q_INVOKABLE QString initRegistryJson() override;
    Q_INVOKABLE QString getRegistryJson() override;

    // Test-only knob: override the delivery topic used for broadcasts.
    // Intentionally not in chronicle_interface.h — the UI's auto-generated
    // proxy doesn't see this method, so production callers can't change the
    // topic. Smoke tests use it for run-isolation (each run picks its own
    // topic so concurrent runs don't cross-contaminate). Pass empty to reset
    // to the compile-time default.
    Q_INVOKABLE QString setBroadcastTopic(const QString& topic);
    Q_INVOKABLE QString getBroadcastTopic();

signals:
    void eventResponse(const QString& eventName, const QVariantList& args);

private:
    struct PendingUpload {
        QString uploadId;
        QString sessionId;
        QString status;
        QString stagedPath;
        QString stagingDir;
        QString contentType;
        QString title;
        QString description;
        QStringList tags;
        QString cid;
        QString metadataHash;
        QJsonObject envelope;
        QString errorCode;
        QString error;
        QString lastError;
        QString publishId;
        qint64 sizeBytes = 0;
        qint64 timestamp = 0;
        qint64 deadlineAtMs = 0;
        qint64 nextRetryAtMs = 0;
        qint64 attemptTimeoutMs = 0;
        int attempt = 0;
    };

    struct PendingPublish {
        QString publishId;
        QString uploadId;
        QString broadcastId;
        QString originalPublishId;
        QString status;
        QString errorCode;
        QString error;
        QString contentType;
        QString title;
        QString description;
        QStringList tags;
        QString cid;
        qint64 sizeBytes = 0;
        QString metadataHash;
        QJsonObject envelope;
        qint64 createdAtMs = 0;
        qint64 updatedAtMs = 0;
        bool broadcastRequested = true;
    };

    struct PendingBroadcast {
        QString broadcastId;
        QString status;
        QString topic;
        QString cid;
        QString metadataHash;
        QString dedupeKey;
        QString errorCode;
        QString error;
        QJsonObject envelope;
        qint64 createdAtMs = 0;
        qint64 updatedAtMs = 0;
        bool deduped = false;
    };

    LogosResult prepareStagedUpload(const QString& path,
                                    const QString& contentType,
                                    const QString& title,
                                    PendingUpload* pending) const;
    void ensureStorageModule();
    void ensureStorageEventSubscription();
    bool ensureStorageReady(QString* error);
    void ensureDeliveryModule();
    bool ensureDeliveryObject(QString* error);
    bool ensureDeliveryReady(QString* error);
    void startBroadcastSend(const QString& broadcastId);
    void startStagedUpload(const QString& uploadId);
    void handleStagedUploadFailure(const QString& uploadId,
                                   const QString& code,
                                   const QString& error,
                                   bool retryable);
    void handleStagedUploadTimeout(const QString& uploadId,
                                   qint64 attemptStartedAtMs,
                                   int attempt);
    void scheduleStagedUploadRetry(const QString& uploadId,
                                   const QString& lastError);
    void clearActiveUpload(const QString& uploadId);
    void scheduleNextQueuedUpload();
    void handleStorageUploadDone(const QVariantList& args);
    void cleanupUploadFiles(const PendingUpload& pending) const;
    QString pendingToJson(const PendingUpload& pending) const;
    QString broadcastToJson(const PendingBroadcast& pending) const;
    QString publishToJson(const PendingPublish& publish) const;
    QString errorToJson(const QString& code, const QString& error) const;
    void advancePublishAfterUpload(const QString& publishId);
    void advancePublishAfterBroadcast(const QString& broadcastId);
    void persistPublishRecord(const PendingPublish& publish);
    void loadPublishLedger();
    QString publishLedgerPath() const;

    LogosAPI* m_logosAPI = nullptr;
    LogosAPIClient* m_storageClient = nullptr;
    LogosAPIClient* m_deliveryClient = nullptr;
    LogosObject* m_storageEvents = nullptr;
    LogosObject* m_deliveryObject = nullptr;
    bool m_storageEventsSubscribed = false;
    bool m_storageInitialized = false;
    bool m_storageStarted = false;
    bool m_deliveryObjectReady = false;
    bool m_deliveryNodeCreated = false;
    bool m_deliveryStarted = false;
    QString m_activeUploadId;
    QHash<QString, PendingUpload> m_uploads;
    QHash<QString, PendingBroadcast> m_broadcasts;
    QHash<QString, QString> m_sessionToUploadId;
    QSet<QString> m_broadcastDedupe;
    QHash<QString, PendingPublish> m_publishes;
    QHash<QString, QString> m_publishDedupe;        // dedupeKey -> publishId
    QHash<QString, QString> m_broadcastToPublishId; // broadcastId -> publishId

    // ── Anchor config (phase 1 — see chronicle_anchor_config.h) ─────────────
    AnchorConfig m_anchorConfig;
    bool m_anchorConfigLoaded = false;

    // Effective broadcast topic. Defaults to CHRONICLE_TOPIC; overridable
    // by smoke tests via setBroadcastTopic.
    QString m_broadcastTopic;

    // ── Anchor ledger (persisted state, source of truth for the UI) ─────────
    struct AnchorRecord {
        QString publishId;
        QString cid;
        QString metadataHash;
        QString state;        // "confirmed" | "failed" — only terminal states persist
        QString txHash;
        QString code;
        QString error;
        qint64  attemptedAtMs = 0;
        qint64  confirmedAtMs = 0;
    };
    QHash<QString, AnchorRecord> m_anchors;
    bool m_anchorsLoaded = false;
    ChronicleAnchorClient* m_anchorClient = nullptr;

    QString anchorLedgerPath() const;
    void    loadAnchorLedger();
    void    persistAnchorRecord(const AnchorRecord& record);
    QJsonObject anchorRecordToJson(const AnchorRecord& record) const;
};

#endif // CHRONICLE_PLUGIN_H
