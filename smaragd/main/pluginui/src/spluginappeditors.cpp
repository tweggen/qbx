#include "app/pluginui/spluginappeditors.h"

#include "app/objects/track/spluginslot.h"
#include "tw/core/twlog.h"
#include "tw/plugins/twplugindescriptor.h"

#include <QHash>
#include <QStringList>

namespace {

// Function-local, so a registration from another module's static initializer
// never meets an unconstructed table (the static-init-order rule
// SActionRegistry::instance() follows).
QHash<QString, spluginappeditors::Entry> &table()
{
    static QHash<QString, spluginappeditors::Entry> t;
    return t;
}

}  // namespace

namespace spluginappeditors {

void registerEditor( const QString &uid, const Entry &entry )
{
    if( uid.isEmpty() || !entry.open ) return;
    if( table().contains( uid ) )
        TW_LOGW( "pluginui", "app editor for uid '%s' registered twice; the later one wins",
                 uid.toStdString().c_str() );
    table().insert( uid, entry );
}

const Entry *entryFor( const SPluginSlot *slot )
{
    if( !slot ) return nullptr;
    const QString uid = QString::fromStdString( slot->getDescriptor().uid );
    auto it = table().constFind( uid );
    return it == table().constEnd() ? nullptr : &it.value();
}

bool hasEditorFor( const SPluginSlot *slot ) { return entryFor( slot ) != nullptr; }

bool openFor( STrack *track, SPluginSlot *slot, QWidget *parentForPosition, bool showWindow )
{
    const Entry *e = entryFor( slot );
    return e && e->open && e->open( track, slot, parentForPosition, showWindow );
}

bool isOpenFor( SPluginSlot *slot )
{
    const Entry *e = entryFor( slot );
    return e && e->isOpen && e->isOpen( slot );
}

void closeFor( SPluginSlot *slot )
{
    const Entry *e = entryFor( slot );
    if( e && e->close ) e->close( slot );
}

QStringList registeredUids()
{
    QStringList out = table().keys();
    out.sort();
    return out;
}

}  // namespace spluginappeditors
