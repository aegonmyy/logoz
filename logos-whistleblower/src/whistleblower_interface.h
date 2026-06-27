#ifndef WHISTLEBLOWER_INTERFACE_H
#define WHISTLEBLOWER_INTERFACE_H

#include <QObject>
#include <QString>
#include "interface.h"

class WhistleblowerInterface : public PluginInterface
{
public:
    virtual ~WhistleblowerInterface() = default;
};

#define WhistleblowerInterface_iid "org.logos.WhistleblowerInterface"
Q_DECLARE_INTERFACE(WhistleblowerInterface, WhistleblowerInterface_iid)

#endif // WHISTLEBLOWER_INTERFACE_H
