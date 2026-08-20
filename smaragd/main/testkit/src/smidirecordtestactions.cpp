#include "app/testkit/smidirecordtestactions.h"

#include <vector>

#include <QDebug>
#include <QDomElement>

#include "app/actions/sactionregistry.h"
#include "app/model/slink.h"
#include "app/model/sobjectpath.h"
#include "app/model/splacements.h"
#include "app/model/sproject.h"
#include "app/shell/sapplication.h"
#include "app/shell/smidirecorder.h"

using namespace strackpath;

QStringList SAssertMidiRecordedAction::knownAttributes() const
{
    return { QStringLiteral( "trackPath" ),      QStringLiteral( "clips" ),
             QStringLiteral( "takes" ),          QStringLiteral( "minTakes" ),
             QStringLiteral( "passes" ),         QStringLiteral( "minPasses" ),
             QStringLiteral( "notes" ),          QStringLiteral( "minNotes" ),
             QStringLiteral( "events" ),         QStringLiteral( "startFrame" ),
             QStringLiteral( "startTolerance" ), QStringLiteral( "durationFrames" ),
             QStringLiteral( "durationTolerance" ),
             QStringLiteral( "mode" ),           QStringLiteral( "quantize" ),
             QStringLiteral( "contains" ) };
}

SApplyResult SAssertMidiRecordedAction::apply( SProject *project )
{
    if( !project ) return { false, nullptr };
    SObject *mixer = splacements::rootNamed( project, pathRoot_ );
    SObject *lane  = splacements::laneAt( mixer, trackPath_ );
    if( !lane ) {
        qWarning() << "assert-midi-recorded: no lane at" << qualifiedToString( pathRoot_, trackPath_ );
        return { false, nullptr };
    }

    // Every EVENT column on the lane. An audio clip beside them is not a
    // failure and is not counted - a track can legitimately hold both.
    std::vector<SLink *> clips;
    for( int i = 0; i < lane->childCount(); ++i ) {
        SLink *lk = lane->childAt( i );
        if( !lk || lk->getSObject().isPathContainer() ) continue;
        if( !lk->getSObject().hasDuration() ) continue;
        if( lk->getSObject().contentKind() != SContentKind::Event ) continue;
        clips.push_back( lk );
    }

    if( clips_ != kUnset && (qint64) clips.size() != clips_ ) {
        qWarning() << "assert-midi-recorded: lane holds" << (int) clips.size()
                   << "event clip(s), expected" << clips_;
        return { false, nullptr };
    }

    SMidiRecorder *rec = SApplication::app().midiRecorder();

    if( !clips.empty() ) {
        SObject &obj = clips.front()->getSObject();
        const qint64 takes = obj.windowTakeCount() > 0
                                 ? (qint64) obj.windowTakeCount() : 1;
        if( takes_ != kUnset && takes != takes_ ) {
            qWarning() << "assert-midi-recorded: column holds" << takes
                       << "take(s), expected" << takes_;
            return { false, nullptr };
        }
        if( minTakes_ != kUnset && takes < minTakes_ ) {
            qWarning() << "assert-midi-recorded: column holds" << takes
                       << "take(s), expected at least" << minTakes_;
            return { false, nullptr };
        }
        // ONE TAKE PER PASS - the whole claim of loop recording, and the half
        // of it that is NOT wall-clock-dependent. Asserted whenever more than
        // one pass was committed, without being asked, exactly as
        // assert-recorded-clip does for audio.
        if( rec && !rec->isActive() && rec->lastPassCount() > 1
            && rec->lastTrackCount() == 1 && clips.size() == 1
            && takes != (qint64) rec->lastPassCount() ) {
            qWarning() << "assert-midi-recorded:" << rec->lastPassCount()
                       << "pass(es) committed but the column holds" << takes
                       << "take(s) -- loop recording must be one take per pass";
            return { false, nullptr };
        }

        const qint64 at  = (qint64) clips.front()->getStartTime();
        const qint64 dur = (qint64) obj.getDurationBlocking();
        if( startFrame_ != kUnset && qAbs( at - startFrame_ ) > startTolerance_ ) {
            qWarning() << "assert-midi-recorded: clip starts at" << at
                       << "expected" << startFrame_ << "+-" << startTolerance_;
            return { false, nullptr };
        }
        if( durationFrames_ != kUnset
            && qAbs( dur - durationFrames_ ) > durationTolerance_ ) {
            qWarning() << "assert-midi-recorded: clip duration" << dur
                       << "expected" << durationFrames_
                       << "+-" << durationTolerance_;
            return { false, nullptr };
        }
    } else if( clips_ == kUnset ) {
        qWarning() << "assert-midi-recorded: no event clip on the lane";
        return { false, nullptr };
    }

    if( !rec ) {
        if( passes_ != kUnset || minPasses_ != kUnset || notes_ != kUnset
            || minNotes_ != kUnset || events_ != kUnset || !mode_.isEmpty()
            || !quantize_.isEmpty() || !contains_.isEmpty() ) {
            qWarning() << "assert-midi-recorded: no MIDI recorder to read from";
            return { false, nullptr };
        }
        return { true, nullptr };
    }

    if( passes_ != kUnset && (qint64) rec->lastPassCount() != passes_ ) {
        qWarning() << "assert-midi-recorded: committed" << rec->lastPassCount()
                   << "pass(es), expected" << passes_;
        return { false, nullptr };
    }
    if( minPasses_ != kUnset && (qint64) rec->lastPassCount() < minPasses_ ) {
        qWarning() << "assert-midi-recorded: committed" << rec->lastPassCount()
                   << "pass(es), expected at least" << minPasses_;
        return { false, nullptr };
    }
    if( notes_ != kUnset && (qint64) rec->lastNoteCount() != notes_ ) {
        qWarning() << "assert-midi-recorded: committed" << rec->lastNoteCount()
                   << "note(s), expected" << notes_;
        return { false, nullptr };
    }
    if( minNotes_ != kUnset && (qint64) rec->lastNoteCount() < minNotes_ ) {
        qWarning() << "assert-midi-recorded: committed" << rec->lastNoteCount()
                   << "note(s), expected at least" << minNotes_;
        return { false, nullptr };
    }
    if( events_ != kUnset && (qint64) rec->lastEventCount() != events_ ) {
        qWarning() << "assert-midi-recorded: committed" << rec->lastEventCount()
                   << "non-note event(s), expected" << events_;
        return { false, nullptr };
    }
    if( !mode_.isEmpty()
        && SMidiRecorder::modeToString( rec->mode() ) != mode_ ) {
        qWarning() << "assert-midi-recorded: mode is"
                   << SMidiRecorder::modeToString( rec->mode() )
                   << "expected" << mode_;
        return { false, nullptr };
    }
    if( !quantize_.isEmpty() && rec->quantizeGrid() != quantize_ ) {
        qWarning() << "assert-midi-recorded: quantize is" << rec->quantizeGrid()
                   << "expected" << quantize_;
        return { false, nullptr };
    }
    if( !contains_.isEmpty() && !rec->describe().contains( contains_ ) ) {
        qWarning() << "assert-midi-recorded: describe() =" << rec->describe()
                   << "does not contain" << contains_;
        return { false, nullptr };
    }

    qDebug() << "assert-midi-recorded: OK --" << rec->describe()
             << "clips" << (int) clips.size();
    return { true, nullptr };
}

void SAssertMidiRecordedAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "trackPath", qualifiedToString( pathRoot_, trackPath_ ) );
    if( clips_ != kUnset )  elem.setAttribute( "clips", QString::number( clips_ ) );
    if( takes_ != kUnset )  elem.setAttribute( "takes", QString::number( takes_ ) );
    if( minTakes_ != kUnset )
        elem.setAttribute( "minTakes", QString::number( minTakes_ ) );
    if( passes_ != kUnset )
        elem.setAttribute( "passes", QString::number( passes_ ) );
    if( minPasses_ != kUnset )
        elem.setAttribute( "minPasses", QString::number( minPasses_ ) );
    if( notes_ != kUnset )  elem.setAttribute( "notes", QString::number( notes_ ) );
    if( minNotes_ != kUnset )
        elem.setAttribute( "minNotes", QString::number( minNotes_ ) );
    if( events_ != kUnset )
        elem.setAttribute( "events", QString::number( events_ ) );
    if( startFrame_ != kUnset )
        elem.setAttribute( "startFrame", QString::number( startFrame_ ) );
    if( startTolerance_ != 1024 )
        elem.setAttribute( "startTolerance", QString::number( startTolerance_ ) );
    if( durationFrames_ != kUnset )
        elem.setAttribute( "durationFrames", QString::number( durationFrames_ ) );
    if( durationTolerance_ != 4096 )
        elem.setAttribute( "durationTolerance",
                           QString::number( durationTolerance_ ) );
    if( !mode_.isEmpty() )     elem.setAttribute( "mode", mode_ );
    if( !quantize_.isEmpty() ) elem.setAttribute( "quantize", quantize_ );
    if( !contains_.isEmpty() ) elem.setAttribute( "contains", contains_ );
}

bool SAssertMidiRecordedAction::readXml( const QDomElement &elem, int )
{
    trackPath_         = parseInto( pathRoot_, elem.attribute( "trackPath" ) );
    clips_             = elem.attribute( "clips", "-1" ).toLongLong();
    takes_             = elem.attribute( "takes", "-1" ).toLongLong();
    minTakes_          = elem.attribute( "minTakes", "-1" ).toLongLong();
    passes_            = elem.attribute( "passes", "-1" ).toLongLong();
    minPasses_         = elem.attribute( "minPasses", "-1" ).toLongLong();
    notes_             = elem.attribute( "notes", "-1" ).toLongLong();
    minNotes_          = elem.attribute( "minNotes", "-1" ).toLongLong();
    events_            = elem.attribute( "events", "-1" ).toLongLong();
    startFrame_        = elem.attribute( "startFrame", "-1" ).toLongLong();
    startTolerance_    = elem.attribute( "startTolerance", "1024" ).toLongLong();
    durationFrames_    = elem.attribute( "durationFrames", "-1" ).toLongLong();
    durationTolerance_ = elem.attribute( "durationTolerance", "4096" ).toLongLong();
    mode_              = elem.attribute( "mode", "" );
    quantize_          = elem.attribute( "quantize", "" );
    contains_          = elem.attribute( "contains", "" );
    return true;
}

static const bool s_reg_assert_midi_recorded = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "assert-midi-recorded" ),
        []{ return new SAssertMidiRecordedAction; } ), true );
