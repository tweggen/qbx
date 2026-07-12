#include "app/objects/cut/sduplicateclipaction.h"
#include "app/objects/track/sremoveclipaction.h"
#include "app/objects/track/strackpath.h"
#include "app/model/sproject.h"
#include "app/actions/sactionregistry.h"
#include "app/objects/mixer/sstdmixer.h"
#include "app/objects/track/strack.h"
#include "app/model/slink.h"
#include "app/objects/cut/scut.h"
#include "tw/core/twfraction.h"
#include <QDomElement>
#include <cstring>

using namespace strackpath;

SLink *makeDuplicateClip( SProject *project, SObject &srcObj,
                          STrack *destTrack, offset_t startTime )
{
    if( !project || !destTrack ) return nullptr;
    SCut *copy;
    if( strcmp( srcObj.metaObject()->className(), "SCut" ) == 0 ) {
        SCut *s = static_cast<SCut*>( &srcObj );
        copy = new SCut( project, s->getContent() );   // share the same content
        // Copy the WHOLE window faithfully: grain params (pitch/grain/crossfade)
        // first without rescale, then the window (offset/duration/loop/stretch).
        // startOffset lives in the stretched output domain, so copying it without
        // the stretch would land the copy elsewhere in the source (and unstretched).
        copy->setGrainParamsRaw( s->getGrainParams() );
        copy->setWindow( s->getStartOffset(), s->getDuration(),
                         s->getLoopLength(), s->getStretch() );
    } else {
        copy = new SCut( project, srcObj );             // wrap a raw clip whole
    }
    SLink *link = new SLink( *copy, NULL );
    link->setStartTime( startTime );
    link->setParent( destTrack );
    return link;
}

SDuplicateClipAction::SDuplicateClipAction( const QList<int> &sourceClipPath,
                                            const QList<int> &destTrackPath,
                                            offset_t startTime )
    : sourceClipPath_( sourceClipPath ), destTrackPath_( destTrackPath ),
      startTime_( startTime )
{
}

SApplyResult SDuplicateClipAction::apply( SProject *project )
{
    if( !project || sourceClipPath_.isEmpty() ) {
        return {false, nullptr};
    }
    SStdMixer *mixer = dynamic_cast<SStdMixer*>( project->getRootComponent() );
    if( !mixer ) {
        return {false, nullptr};
    }

    // Resolve the source clip.
    QList<int> srcTrackPath = sourceClipPath_;
    int srcIdx = srcTrackPath.takeLast();
    STrack *srcTrack = dynamic_cast<STrack*>( resolveByPath( mixer, srcTrackPath ) );
    if( !srcTrack ) {
        return {false, nullptr};
    }
    SLink *srcLink = srcTrack->childAt( srcIdx );
    if( !srcLink || dynamic_cast<STrack*>( &srcLink->getSObject() ) ) {
        return {false, nullptr};   // missing, or a nested track lane (not a clip)
    }

    STrack *destTrack = dynamic_cast<STrack*>( resolveByPath( mixer, destTrackPath_ ) );
    if( !destTrack ) {
        return {false, nullptr};
    }

    SLink *copy = makeDuplicateClip( project, srcLink->getSObject(), destTrack, startTime_ );
    if( !copy ) {
        return {false, nullptr};
    }

    QList<int> newClipPath = destTrackPath_;
    newClipPath.append( destTrack->indexOfChild( copy ) );
    SAction *inverse = new SRemoveClipAction( newClipPath, sourceClipPath_,
                                              destTrackPath_, startTime_ );
    return {true, inverse};
}

void SDuplicateClipAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "source", pathToString( sourceClipPath_ ) );
    elem.setAttribute( "destTrack", pathToString( destTrackPath_ ) );
    elem.setAttribute( "startTime", QString::fromStdString( Fraction(startTime_, 1).toString() ) );
}

bool SDuplicateClipAction::readXml( const QDomElement &elem, int /*version*/ )
{
    sourceClipPath_ = stringToPath( elem.attribute( "source" ) );
    destTrackPath_  = stringToPath( elem.attribute( "destTrack" ) );
    startTime_      = (offset_t) parseFractionOrDouble( elem.attribute( "startTime", "0" ).toStdString() ).toDouble();
    return true;
}

static const bool s_reg_duplicateclip = (
    SActionRegistry::instance().registerType(
        QStringLiteral("duplicate-clip"),
        []{ return new SDuplicateClipAction; }
    ), true
);
