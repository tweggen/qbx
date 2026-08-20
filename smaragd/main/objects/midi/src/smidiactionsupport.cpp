#include "app/objects/midi/smidiactionsupport.h"

#include <QDomDocument>
#include <QDomElement>

#include "app/model/sclipwindow.h"
#include "app/model/slink.h"
#include "app/model/splacements.h"
#include "app/model/sobjectpath.h"
#include "app/model/sproject.h"
#include "app/objects/midi/smidicut.h"

namespace smidiactions {

ClipRef resolveClip( SProject *project, const QString &pathRoot_,
                     const QList<int> &clipPath, int take )
{
    ClipRef out;
    if( !project || clipPath.isEmpty() ) return out;
    SObject *mixer = splacements::rootNamed( project, pathRoot_ );
    SLink *link = mixer ? splacements::placementAt( mixer, clipPath ) : nullptr;
    if( !link ) return out;
    out.link = link;

    SObject *obj = &link->getSObject();
    // A stack presents the addressed take through the generic seam; -1 is the
    // active one, which is the resolution every per-clip verb uses.
    if( SClipWindow *w = obj->windowTakeAt( take ) ) obj = &w->asObject();
    out.cut = dynamic_cast<SMidiCut *>( obj );
    if( !out.cut ) { out.link = nullptr; return out; }
    out.seq = out.cut->sequence();
    if( !out.seq ) { out.link = nullptr; out.cut = nullptr; }
    return out;
}

SEvent readNoteChild( const QDomElement &el )
{
    SEvent e;
    e.kind = twEventKind::NoteOn;
    e.t = el.attribute( "tick", "0" ).toLongLong();
    e.dur = el.attribute( "dur", "0" ).toLongLong();
    e.key = el.attribute( "key", "60" ).toInt();
    e.value = el.attribute( "velocity", "100" ).toDouble();
    e.value2 = el.attribute( "releaseVelocity", "64" ).toDouble();
    e.channel = el.attribute( "channel", "0" ).toInt();
    e.muted = el.attribute( "m", "0" ).toInt() != 0;
    return e;
}

void writeNoteChild( QDomElement &parent, const SEvent &e )
{
    QDomElement n = parent.ownerDocument().createElement( "n" );
    n.setAttribute( "tick", QString::number( e.t ) );
    n.setAttribute( "dur", QString::number( e.dur ) );
    n.setAttribute( "key", QString::number( e.key ) );
    n.setAttribute( "velocity", QString::number( e.value ) );
    n.setAttribute( "channel", QString::number( e.channel ) );
    if( e.value2 != 64.0 )
        n.setAttribute( "releaseVelocity", QString::number( e.value2 ) );
    if( e.muted ) n.setAttribute( "m", "1" );
    parent.appendChild( n );
}

void writeEventChildren( QDomElement &parent,
                                       const std::vector<SEvent> &events )
{
    QDomDocument doc = parent.ownerDocument();
    for( const SEvent &e : events ) {
        QDomElement el = doc.createElement( "e" );
        el.setAttribute( "k", smidievents::kindToString( e ) );
        el.setAttribute( "t", QString::number( e.t ) );
        if( e.dur != 0 )      el.setAttribute( "d", QString::number( e.dur ) );
        if( e.channel >= 0 )  el.setAttribute( "ch", QString::number( e.channel ) );
        if( e.key >= 0 )      el.setAttribute( "key", QString::number( e.key ) );
        if( e.paramId != 0 )  el.setAttribute( "p", QString::number( e.paramId ) );
        if( e.value != 0.0 )  el.setAttribute( "v", QString::number( e.value, 'g', 17 ) );
        if( e.value2 != 0.0 ) el.setAttribute( "v2", QString::number( e.value2, 'g', 17 ) );
        if( !e.text.isEmpty() ) el.setAttribute( "text", e.text );
        if( !e.blob.isEmpty() )
            el.setAttribute( "blob", QString::fromLatin1( e.blob.toHex() ) );
        if( e.muted ) el.setAttribute( "m", "1" );
        for( auto it = e.extra.constBegin(); it != e.extra.constEnd(); ++it )
            el.setAttribute( it.key(), it.value() );
        parent.appendChild( el );
    }
}

std::vector<SEvent> readEventChildren( const QDomElement &parent )
{
    std::vector<SEvent> out;
    QDomNode n = parent.firstChild();
    while( !n.isNull() ) {
        if( n.isElement() ) {
            const QDomElement el = n.toElement();
            if( el.tagName() == "e" )
                out.push_back( smidievents::readEvent( el ) );
            else if( el.tagName() == "n" )
                out.push_back( readNoteChild( el ) );
        }
        n = n.nextSibling();
    }
    return out;
}

bool matchesNote( const SEvent &e, qint64 tick, int key,
                                int channel )
{
    if( e.kind != twEventKind::NoteOn ) return false;
    if( e.t != tick ) return false;
    if( key >= 0 && e.key != key ) return false;
    if( channel >= 0 && e.channel != channel ) return false;
    return true;
}

}  // namespace smidiactions
