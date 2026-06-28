#ifndef WHISTLEBLOWER_PLUGIN_H
#define WHISTLEBLOWER_PLUGIN_H

#include <QTimer>
#include <QVariantList>

#include "rep_whistleblower_source.h"
#include "whistleblower_interface.h"
#include "LogosViewPluginBase.h"

class LogosAPI;
class LogosAPIClient;

// WhistleblowerPlugin bridges the QML RemoteObjects layer with Chronicle.
// It owns a single LogosAPIClient that calls Chronicle methods, drives a
// poll loop while a publish job is in flight, and retries the delivery
// bring-up with exponential backoff.
class WhistleblowerPlugin : public WhistleblowerSimpleSource,
                            public WhistleblowerInterface,
                            public WhistleblowerViewPluginBase
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID WhistleblowerInterface_iid FILE "metadata.json")
    Q_INTERFACES(WhistleblowerInterface)

public:
    explicit WhistleblowerPlugin(QObject* parent = nullptr);
    ~WhistleblowerPlugin() override;

    QString name()    const override { return QStringLiteral("whistleblower"); }
    QString version() const override { return QStringLiteral("1.0.0"); }

    Q_INVOKABLE void initLogos(LogosAPI* api);

    // Slots wired by the .rep-generated source base
    void submitDocument(QString path, QString contentType, QString title,
                        QString description, QString tagsCsv)       override;
    QString fetchHistoryJson()  override;
    void connectDelivery()      override;
    void reloadHistory()        override;
    void discardHistory()       override;

    void anchorJob(QString publishId)       override;
    void reloadAnchorCaps()                 override;
    void applyAnchorConfig(QString cfgJson) override;
    void reloadAnchors()                    override;

signals:
    void eventResponse(const QString& eventName, const QVariantList& args);

private slots:
    void tickJobPoll();

private:
    void         ensureClient();
    QString      invokeChronicle(const QString& method,
                                 const QVariantList& args = {});
    void         handleJobResponse(const QString& responseJson);
    void         clearJobState();
    void         scheduleDeliveryRetry();

    LogosAPI*       m_api            = nullptr;
    LogosAPIClient* m_client         = nullptr;
    QTimer*         m_pollTimer      = nullptr;
    QTimer*         m_deliveryRetry  = nullptr;
    int             m_deliveryTries  = 0;
};

#endif // WHISTLEBLOWER_PLUGIN_H
