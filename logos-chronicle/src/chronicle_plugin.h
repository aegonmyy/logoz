#ifndef CHRONICLE_PLUGIN_H
#define CHRONICLE_PLUGIN_H

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

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

    QString name()    const override { return QStringLiteral("chronicle"); }
    QString version() const override { return QStringLiteral("1.0.0"); }

    Q_INVOKABLE void initLogos(LogosAPI* api);

    Q_INVOKABLE QString health()                                    override;
    Q_INVOKABLE QString uploadFileJson(const QString& path,
                                       const QString& contentType,
                                       const QString& title)        override;
    Q_INVOKABLE QString uploadStatusJson(const QString& id)         override;
    Q_INVOKABLE QString normalizeContentTypeJson(const QString& ct) override;
    Q_INVOKABLE QString hashMetadataJson(const QString& contentType,
                                         const QString& sizeBytes,
                                         const QString& title,
                                         const QString& description,
                                         const QString& tagsJson)   override;
    Q_INVOKABLE QString buildMetadataEnvelopeJson(
                             const QString& inputJson)               override;
    Q_INVOKABLE QString startBroadcasterJson()                      override;
    Q_INVOKABLE QString broadcastEnvelopeJson(
                             const QString& envelopeJson)            override;
    Q_INVOKABLE QString broadcastStatusJson(const QString& id)      override;
    Q_INVOKABLE QString publishFileJson(const QString& reqJson)     override;
    Q_INVOKABLE QString publishStatusJson(const QString& id)        override;
    Q_INVOKABLE QString listPublishedJson()                         override;
    Q_INVOKABLE QString clearPublishedJson()                        override;
    Q_INVOKABLE QString anchorCapabilitiesJson()                    override;
    Q_INVOKABLE QString getAnchorConfigJson()                       override;
    Q_INVOKABLE QString setAnchorConfigJson(const QString& cfgJson) override;
    Q_INVOKABLE QString anchorBatchJson(const QString& reqJson)     override;
    Q_INVOKABLE QString anchorStatusJson(const QString& id)         override;
    Q_INVOKABLE QString lookupAnchorJson(const QString& cid)        override;
    Q_INVOKABLE QString listAnchorsJson()                           override;
    Q_INVOKABLE QString clearAnchorsJson()                          override;
    Q_INVOKABLE QString initRegistryJson()                          override;
    Q_INVOKABLE QString getRegistryJson()                           override;

    // Test-only: override broadcast topic for smoke-test isolation
    Q_INVOKABLE QString setBroadcastTopic(const QString& topic);
    Q_INVOKABLE QString getBroadcastTopic();

signals:
    void eventResponse(const QString& name, const QVariantList& args);

private:
    // ── Job tracking ────────────────────────────────────────────────────────
    // A single Job struct covers uploads, broadcasts, and publishes to keep
    // state management in one place.

    struct UploadJob {
        QString id;
        QString status;          // staging|uploading|done|error
        QString path;
        QString contentType;
        QString title;
        QString description;
        QStringList tags;
        QString cid;
        QString metaHash;
        QJsonObject envelope;
        QString errCode;
        QString errMsg;
        qint64  sizeBytes   = 0;
        qint64  startMs     = 0;
        qint64  deadlineMs  = 0;
        qint64  nextRetryMs = 0;
        int     attempt     = 0;
        QString sessionId;
        QString publishId;       // publish job this upload feeds (afterUpload)
        QString stagingDir;
        QString stagedPath;
    };

    struct BroadcastJob {
        QString id;
        QString status;          // pending|sent|deduped|error
        QString cid;
        QString metaHash;
        QString topic;
        QJsonObject envelope;
        QString errCode;
        QString errMsg;
        bool    deduped = false;
        qint64  createdMs = 0;
    };

    struct PublishJob {
        QString id;
        QString status;          // uploading|broadcasting|done|error
        QString uploadId;
        QString broadcastId;
        QString cid;
        QString metaHash;
        QString contentType;
        QString title;
        QString description;
        QStringList tags;
        qint64  sizeBytes = 0;
        QJsonObject envelope;
        QString errCode;
        QString errMsg;
        qint64  createdMs = 0;
        qint64  updatedMs = 0;
        bool    wantBroadcast = true;
    };

    struct AnchorRecord {
        QString publishId;
        QString cid;
        QString metaHash;
        QString state;           // "confirmed" | "failed"
        QString txHash;
        QString errCode;
        QString errMsg;
        qint64  attemptedMs  = 0;
        qint64  confirmedMs  = 0;
    };

    // ── Helpers ─────────────────────────────────────────────────────────────
    void         initStorage();
    void         initDelivery();
    bool         storageReady(QString* err);
    bool         deliveryReady(QString* err);
    void         startUpload(const QString& uploadId);
    void         onUploadDone(const QVariantList& args);
    void         failUpload(const QString& id, const QString& code,
                             const QString& msg, bool retryable);
    void         scheduleRetry(const QString& uploadId, const QString& lastErr);
    void         sendBroadcast(const QString& broadcastId);
    void         afterUpload(const QString& publishId);
    void         afterBroadcast(const QString& broadcastId);
    void         savePublish(const PublishJob& job);
    void         loadPublishLedger();
    QString      publishLedgerPath() const;
    void         loadAnchorLedger();
    void         saveAnchor(const AnchorRecord& rec);
    QString      anchorLedgerPath() const;
    QString      uploadToJson(const UploadJob& j)     const;
    QString      broadcastToJson(const BroadcastJob& j) const;
    QString      publishToJson(const PublishJob& j)   const;
    QJsonObject  anchorToJson(const AnchorRecord& r)  const;
    QString      errJson(const QString& code, const QString& msg) const;

    // ── State ────────────────────────────────────────────────────────────────
    LogosAPI*       m_api             = nullptr;
    LogosAPIClient* m_storageClient   = nullptr;
    LogosAPIClient* m_deliveryClient  = nullptr;
    LogosObject*    m_storageEvents   = nullptr;
    LogosObject*    m_deliveryObj     = nullptr;
    bool m_storageSubscribed  = false;
    bool m_storageReady       = false;
    bool m_deliveryObjReady   = false;
    bool m_deliveryNodeUp     = false;
    bool m_deliveryStarted    = false;

    QString m_activeUploadId;
    QHash<QString, UploadJob>    m_uploads;
    QHash<QString, BroadcastJob> m_broadcasts;
    QHash<QString, PublishJob>   m_publishes;
    QHash<QString, QString>      m_sessionToUpload;
    QSet<QString>                m_bcastDedupe;
    QHash<QString, QString>      m_publishDedupe;    // dedupeKey → publishId
    QHash<QString, QString>      m_bcastToPublish;   // broadcastId → publishId

    AnchorConfig                  m_anchorCfg;
    bool                          m_anchorCfgLoaded = false;
    QHash<QString, AnchorRecord>  m_anchors;
    bool                          m_anchorsLoaded   = false;
    FfiClient*                    m_ffi             = nullptr;

    QString m_broadcastTopic;
};

#endif // CHRONICLE_PLUGIN_H
