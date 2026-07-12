#include "app/model/sdetaileditors.h"
#include "app/model/sobject.h"

#include <QHash>
#include <QString>

namespace sdetaileditors {

static QHash<QString, Factory> &registry()
{
    static QHash<QString, Factory> map;   // immune to init-order
    return map;
}

void registerEditor( const char *className, Factory factory )
{
    registry().insert( QString::fromLatin1( className ), factory );
}

QWidget *create( SObject &obj, QWidget *parent )
{
    Factory f = registry().value(
        QString::fromLatin1( obj.metaObject()->className() ) );
    return f ? f( obj, parent ) : nullptr;
}

}  // namespace sdetaileditors
