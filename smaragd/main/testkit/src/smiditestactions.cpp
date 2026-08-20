#include "app/testkit/smiditestactions.h"

#include <QDebug>
#include <QDomElement>
#include <cmath>
#include <string>
#include <vector>

#include "app/actions/sactionregistry.h"
#include "app/model/sobjectpath.h"
#include "app/model/splacements.h"
#include "app/model/sproject.h"
#include "app/objects/midi/smidiactionsupport.h"
#include "app/objects/midi/smidicut.h"
#include "app/objects/midi/smidisequence.h"
#include "app/objects/track/strack.h"
#include "app/testkit/stestfilepath.h"
#include "app/shell/sapplication.h"
#include "tw/events/tweventsource.h"
#include "tw/events/twsmf.h"

using namespace strackpath;

namespace {

/**
 * A kind filter. `noteoff-synth` is deliberately its OWN spelling rather than
 * `noteoff` plus a flag attribute: a synthesised release is the thing the
 * non-destructive split has to prove, and giving it a name makes the assertion
 * read like the rule it gates.
 */
bool kindMatches( const QString &spec, const twEvent &e )
{
    if( spec.isEmpty() ) return true;
    const QString s = spec.toLower();
    if( s == "note" || s == "noteon" ) return e.kind == twEventKind::NoteOn;
    if( s == "noteoff" ) return e.kind == twEventKind::NoteOff;
    if( s == "noteoff-synth" )
        return e.kind == twEventKind::NoteOff
            && ( e.flags & twEventSynthesisedOff ) != 0;
    if( s == "noteoff-real" )
        return e.kind == twEventKind::NoteOff
            && ( e.flags & twEventSynthesisedOff ) == 0;
    if( s == "cc" ) return e.kind == twEventKind::ControlChange;
    if( s == "bend" ) return e.kind == twEventKind::PitchBend;
    if( s == "program" ) return e.kind == twEventKind::ProgramChange;
    if( s == "sysex" ) return e.kind == twEventKind::Sysex;
    if( s == "tempo" ) return e.kind == twEventKind::Tempo;
    if( s == "timesig" ) return e.kind == twEventKind::TimeSig;
    if( s == "meta" ) return twEventIsMetadata( e.kind );
    if( s == "any" ) return true;
    return false;
}

QString describeEvent( const twEvent &e )
{
    return QString( "t=%1 kind=%2 ch=%3 key=%4 v=%5 dur=%6%7" )
        .arg( (qlonglong) e.time ).arg( (int) e.kind ).arg( e.channel )
        .arg( e.key ).arg( e.value ).arg( (qlonglong) e.duration )
        .arg( ( e.flags & twEventSynthesisedOff ) ? " synth" : "" );
}

}  // namespace

// ---------------------------------------------------------------------------
// assert-midi-events
// ---------------------------------------------------------------------------

QStringList SAssertMidiEventsAction::knownAttributes() const
{
    return { "scope", "clip", "trackPath", "take", "kind", "at", "tolerance",
             "key", "velocity", "velocityTolerance", "channel", "count",
             "minCount", "maxCount", "startFrame", "frameCount", "contains" };
}

SApplyResult SAssertMidiEventsAction::apply( SProject *project )
{
    if( !project ) return { false, nullptr };

    std::vector<twEvent> events;
    QString what;

    if( scope_.compare( "feed", Qt::CaseInsensitive ) == 0 ) {
        STrack *track = dynamic_cast<STrack *>( splacements::laneAt(
            splacements::rootNamed( project, pathRoot_ ), stringToPath( trackPath_ ) ) );
        if( !track ) {
            qWarning() << "assert-midi-events: no track at" << trackPath_;
            return { false, nullptr };
        }
        const int64_t len = frameCount_ >= 0
            ? frameCount_
            : ( (int64_t) project->getDurationFrames() > 0
                    ? (int64_t) project->getDurationFrames()
                    : (int64_t) project->getSRate() * 60 );
        twEventBlock block;
        // The FEED, not the clip set: this is the merge the instrument will
        // read (design 3.2.1), so mute, solo and midiRouting are visible here
        // and nowhere else.
        track->eventFeed()->collect( startFrame_, len, block );
        events = block.events;
        what = QString( "feed of track %1 over [%2, %3)" )
                   .arg( trackPath_ ).arg( (qlonglong) startFrame_ )
                   .arg( (qlonglong) ( startFrame_ + len ) );
    } else {
        smidiactions::ClipRef ref = smidiactions::resolveClip( project, pathRoot_, stringToPath( clip_ ), take_ );
        if( !ref.valid() ) {
            qWarning() << "assert-midi-events: no MIDI clip at" << clip_;
            return { false, nullptr };
        }
        const SMidiCutSnapshot snap = ref.cut->getSnapshot();
        if( kind_.compare( "noteoff-synth", Qt::CaseInsensitive ) == 0
         || kind_.compare( "noteoff", Qt::CaseInsensitive ) == 0
         || kind_.compare( "noteoff-real", Qt::CaseInsensitive ) == 0 ) {
            // A note-off does not exist in a table - notes are stored WITH
            // their length. It is INVENTED by a collect at the clip end, which
            // is exactly the thing worth asserting, so this scope quietly
            // upgrades to a collect over the clip's own window.
            twEventClipSet set;
            SMidiCut *cut = ref.cut;
            set.insertClip( cut, 0, snap.durationFrames,
                            [cut]( offset_t p ) { return cut->resolveEventClip( p ); } );
            twEventBlock block;
            // +1: windows are half-open, and the clip-end note-off lands at
            // OFFSET durationFrames (events/CONTRACT inv. 9 - a boundary that
            // falls exactly on a window edge belongs to the window that starts
            // there). A collect over [0, duration) would therefore never show
            // the very release this assertion exists to see.
            const int64_t len = frameCount_ >= 0
                ? frameCount_ : (int64_t) snap.durationFrames + 1;
            set.collect( startFrame_, len, block );
            events = block.events;
        } else {
            if( !snap.framesSeq ) {
                qWarning() << "assert-midi-events: clip" << clip_
                           << "has no event table";
                return { false, nullptr };
            }
            // Clip-relative frames, so an assertion reads the same wherever the
            // clip is placed: the snapshot is content-zero based, the window
            // starts at startOffsetFrames.
            for( const twEvent &e : snap.framesSeq->events() ) {
                if( e.time < snap.startOffsetFrames ) continue;
                if( e.time >= snap.startOffsetFrames + snap.durationFrames )
                    continue;
                twEvent shifted = e;
                shifted.time = e.time - snap.startOffsetFrames;
                events.push_back( shifted );
            }
        }
        what = QString( "clip %1" ).arg( clip_ );
    }

    QStringList dump;
    int matched = 0;
    for( const twEvent &e : events ) {
        dump << describeEvent( e );
        if( !kindMatches( kind_, e ) ) continue;
        if( at_ >= 0 && std::llabs( (long long) ( e.time - at_ ) ) > tolerance_ )
            continue;
        if( key_ >= 0 && e.key != key_ ) continue;
        if( channel_ >= 0 && e.channel != channel_ ) continue;
        if( velocity_ >= 0.0
            && std::fabs( e.value - velocity_ ) > velocityTolerance_ ) continue;
        ++matched;
    }

    const QString detail =
        QString( "%1: %2 matching event(s) [kind=%3 at=%4 key=%5 ch=%6]; all: %7" )
            .arg( what ).arg( matched ).arg( kind_.isEmpty() ? "any" : kind_ )
            .arg( (qlonglong) at_ ).arg( key_ ).arg( channel_ )
            .arg( dump.join( " | " ) );

    if( count_ >= 0 && matched != count_ ) {
        qWarning() << "assert-midi-events FAILED: expected count" << count_
                   << "-" << detail;
        return { false, nullptr };
    }
    if( minCount_ >= 0 && matched < minCount_ ) {
        qWarning() << "assert-midi-events FAILED: expected at least" << minCount_
                   << "-" << detail;
        return { false, nullptr };
    }
    if( maxCount_ >= 0 && matched > maxCount_ ) {
        qWarning() << "assert-midi-events FAILED: expected at most" << maxCount_
                   << "-" << detail;
        return { false, nullptr };
    }
    if( !contains_.isEmpty() && !dump.join( " | " ).contains( contains_ ) ) {
        qWarning() << "assert-midi-events FAILED: dump does not contain"
                   << contains_ << "-" << detail;
        return { false, nullptr };
    }
    qDebug() << "assert-midi-events: OK -" << detail;
    return { true, nullptr };   // assertions are not undoable
}

void SAssertMidiEventsAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "scope", scope_ );
    if( !clip_.isEmpty() )      elem.setAttribute( "clip", clip_ );
    if( !trackPath_.isEmpty() ) elem.setAttribute( "trackPath", trackPath_ );
    elem.setAttribute( "take", take_ );
    if( !kind_.isEmpty() )      elem.setAttribute( "kind", kind_ );
    elem.setAttribute( "at", QString::number( at_ ) );
    elem.setAttribute( "tolerance", QString::number( tolerance_ ) );
    elem.setAttribute( "key", key_ );
    elem.setAttribute( "velocity", QString::number( velocity_ ) );
    if( velocityTolerance_ != 0.5 )
        elem.setAttribute( "velocityTolerance",
                           QString::number( velocityTolerance_ ) );
    elem.setAttribute( "channel", channel_ );
    elem.setAttribute( "count", count_ );
    elem.setAttribute( "minCount", minCount_ );
    elem.setAttribute( "maxCount", maxCount_ );
    elem.setAttribute( "startFrame", QString::number( startFrame_ ) );
    elem.setAttribute( "frameCount", QString::number( frameCount_ ) );
    if( !contains_.isEmpty() )  elem.setAttribute( "contains", contains_ );
}

bool SAssertMidiEventsAction::readXml( const QDomElement &elem, int )
{
    scope_ = elem.attribute( "scope", "clip" );
    clip_ = elem.attribute( "clip", "" );
    trackPath_ = elem.attribute( "trackPath", "" );
    take_ = elem.attribute( "take", "-1" ).toInt();
    kind_ = elem.attribute( "kind", "" );
    at_ = elem.attribute( "at", "-1" ).toLongLong();
    tolerance_ = elem.attribute( "tolerance", "0" ).toLongLong();
    key_ = elem.attribute( "key", "-1" ).toInt();
    velocity_ = elem.attribute( "velocity", "-1" ).toDouble();
    velocityTolerance_ = elem.attribute( "velocityTolerance", "0.5" ).toDouble();
    channel_ = elem.attribute( "channel", "-1" ).toInt();
    count_ = elem.attribute( "count", "-1" ).toInt();
    minCount_ = elem.attribute( "minCount", "-1" ).toInt();
    maxCount_ = elem.attribute( "maxCount", "-1" ).toInt();
    startFrame_ = elem.attribute( "startFrame", "0" ).toLongLong();
    frameCount_ = elem.attribute( "frameCount", "-1" ).toLongLong();
    contains_ = elem.attribute( "contains", "" );
    return true;
}

static const bool s_reg_assert_midi_events = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "assert-midi-events" ),
        []{ return new SAssertMidiEventsAction; } ), true );

// ---------------------------------------------------------------------------
// assert-midi-file
// ---------------------------------------------------------------------------

QStringList SAssertMidiFileAction::knownAttributes() const
{
    return { "filename", "trackCount", "noteCount", "eventCount", "firstTick",
             "ppq", "format" };
}

SApplyResult SAssertMidiFileAction::apply( SProject *project )
{
    if( filename_.isEmpty() ) {
        qWarning() << "assert-midi-file: filename is required";
        return { false, nullptr };
    }
    const QString outputDir = SApplication::app().testOutputDir();
    QStringList tried;
    const QString path =
        resolveTestFilePath( filename_, outputDir, project, &tried );

    twSmfFile file;
    std::string err;
    if( !twSmf::readFile( path.toStdString(), file, &err ) ) {
        qWarning() << "assert-midi-file: cannot read" << path << "-"
                   << QString::fromStdString( err ) << "( tried" << tried << ")";
        return { false, nullptr };
    }

    int notes = 0, events = 0;
    qint64 firstTick = -1;
    for( const twSmfTrack &tr : file.tracks ) {
        if( !tr.events ) continue;
        events += (int) tr.events->size();
        for( const twEvent &e : tr.events->events() ) {
            if( e.kind == twEventKind::NoteOn ) ++notes;
            if( firstTick < 0 || e.time < firstTick ) firstTick = e.time;
        }
    }
    if( firstTick < 0 ) firstTick = 0;

    struct Check { const char *what; long long want, got; };
    const Check checks[] = {
        { "trackCount", trackCount_, (long long) file.tracks.size() },
        { "noteCount",  noteCount_,  notes },
        { "eventCount", eventCount_, events },
        { "firstTick",  firstTick_,  firstTick },
        { "ppq",        ppq_,        file.ppq },
        { "format",     format_,     file.format },
    };
    for( const Check &c : checks ) {
        if( c.want < 0 ) continue;
        if( c.want != c.got ) {
            qWarning() << "assert-midi-file FAILED:" << path << c.what
                       << "expected" << c.want << "got" << c.got;
            return { false, nullptr };
        }
    }
    qDebug() << "assert-midi-file: OK -" << path << "tracks"
             << (int) file.tracks.size() << "notes" << notes << "events"
             << events << "ppq" << file.ppq << "format" << file.format;
    return { true, nullptr };
}

void SAssertMidiFileAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "filename", filename_ );
    elem.setAttribute( "trackCount", trackCount_ );
    elem.setAttribute( "noteCount", noteCount_ );
    elem.setAttribute( "eventCount", eventCount_ );
    elem.setAttribute( "firstTick", QString::number( firstTick_ ) );
    elem.setAttribute( "ppq", ppq_ );
    elem.setAttribute( "format", format_ );
}

bool SAssertMidiFileAction::readXml( const QDomElement &elem, int )
{
    filename_ = elem.attribute( "filename", "" );
    trackCount_ = elem.attribute( "trackCount", "-1" ).toInt();
    noteCount_ = elem.attribute( "noteCount", "-1" ).toInt();
    eventCount_ = elem.attribute( "eventCount", "-1" ).toInt();
    firstTick_ = elem.attribute( "firstTick", "-1" ).toLongLong();
    ppq_ = elem.attribute( "ppq", "-1" ).toInt();
    format_ = elem.attribute( "format", "-1" ).toInt();
    return true;
}

static const bool s_reg_assert_midi_file = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "assert-midi-file" ),
        []{ return new SAssertMidiFileAction; } ), true );
