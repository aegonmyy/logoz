#ifndef WHISTLEBLOWER_PLUGIN_H
#define WHISTLEBLOWER_PLUGIN_H

#include <QString>
#include <QTimer>
#include <QVariantList>

#include "whistleblower_interface.h"
#include "LogosViewPluginBase.h"
#include "rep_whistleblower_source.h"

class LogosAPI;
class LogosAPIClient;

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

    QString name()    const override { return "whistleblower"; }
    QString version() const override { return "1.0.0"; }

    Q_INVOKABLE void initLogos(LogosAPI* api);

    // Slots from whistleblower.rep
    void publish(QString path,
                 QString contentType,
                 QString title,
                 QString description,
                 QString tagsCsv) override;
    QString listPublishedJson() override;
    void startBroadcaster() override;
    void refreshPublishedList() override;
    void clearHistory() override;

    void anchorPublished(QString publishId) override;
    void refreshAnchorCapabilities() override;
    void setAnchorConfig(QString cfgJson) override;
    void refreshAnchors() override;

signals:
    void eventResponse(const QString& eventName, const QVariantList& args);

private slots:
    void pollPublishStatus();

private:
    void ensureChronicleClient();
    QString callChronicle(const QString& method, const QVariantList& args = {});
    void handlePublishResponse(const QString& responseJson);
    void resetPublishState();

    LogosAPI*       m_logosAPI = nullptr;
    LogosAPIClient* m_chronicleClient = nullptr;
    QTimer*         m_pollTimer = nullptr;
    QTimer*         m_startBroadcasterRetryTimer = nullptr;
    int             m_startBroadcasterAttempts = 0;
};

#endif // WHISTLEBLOWER_PLUGIN_H
