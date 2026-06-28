#ifndef WHISTLEBLOWER_INTERFACE_H
#define WHISTLEBLOWER_INTERFACE_H

#include <QObject>
#include <QString>

#include "interface.h"

// Marker interface for the Whistleblower Logos view plugin.
// All logic lives in WhistleblowerPlugin; this header supplies the IID used by
// Q_PLUGIN_METADATA and Q_DECLARE_INTERFACE.
class WhistleblowerInterface : public PluginInterface
{
public:
    virtual ~WhistleblowerInterface() = default;
};

#define WhistleblowerInterface_iid "org.logos.WhistleblowerInterface/1"
Q_DECLARE_INTERFACE(WhistleblowerInterface, WhistleblowerInterface_iid)

#endif // WHISTLEBLOWER_INTERFACE_H
