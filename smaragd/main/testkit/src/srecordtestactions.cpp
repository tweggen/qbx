#include "app/testkit/srecordtestactions.h"

#include <vector>

#include <QDebug>
#include <QDomElement>
#include <QFileInfo>

#include "app/actions/sactionregistry.h"
#include "app/model/sclipwindow.h"
#include "app/model/slink.h"
#include "app/model/sobjectpath.h"
#include "app/model/splacements.h"
#include "app/model/sproject.h"
#include "app/objects/cut/stakestack.h"
#include "app/shell/sapplication.h"
#include "app/shell/saudiorecorder.h"

#include "tw/analysis/audio_analysis.h"
#include "tw/core/twtypes.h"

using namespace strackpath;

// --- record-start -----------------------------------------------------------

SApplyResult SRecordStartAction::apply( SProject * )
{
    if( !SApplication::app().startRecording() ) {
        SAudioRecorder *r = SApplication::app().audioRecorder();
        qWarning() << "record-start: refused —"
                   << ( r ? r->errorMessage() : QStringLiteral( "no recorder" ) );
        return { false, nullptr };
    }
    return { true, nullptr };   // transport, not undoable
}

void SRecordStartAction::writeXml( QDomElement & ) const {}
bool SRecordStartAction::readXml( const QDomElement &, int ) { return true; }

// --- record-stop ------------------------------------------------------------

SApplyResult SRecordStopAction::apply( SProject * )
{
    if( !SApplication::app().isRecordingActive() ) {
        qWarning() << "record-stop: not recording";
        return { false, nullptr };
    }
    SApplication::app().stopRecording();
    return { true, nullptr };
}

void SRecordStopAction::writeXml( QDomElement & ) const {}
bool SRecordStopAction::readXml( const QDomElement &, int ) { return true; }

// --- assert-recorded-clip ---------------------------------------------------

QStringList SAssertRecordedClipAction::knownAttributes() const
{
    return { QStringLiteral( "trackPath" ),        QStringLiteral( "clips" ),
             QStringLiteral( "takes" ),            QStringLiteral( "startFrame" ),
             QStringLiteral( "startTolerance" ),   QStringLiteral( "durationFrames" ),
             QStringLiteral( "durationTolerance" ),QStringLiteral( "minDurationFrames" ),
             QStringLiteral( "inputLatencyFrames" ),
             QStringLiteral( "outputLatencyFrames" ),
             QStringLiteral( "userOffsetFrames" ), QStringLiteral( "compensationFrames" ),
             QStringLiteral( "trimmedFrames" ),    QStringLiteral( "passes" ),
             QStringLiteral( "growing" ),          QStringLiteral( "previewNonEmpty" ),
             QStringLiteral( "sourceAtStartFrame" ),
             QStringLiteral( "sourceTolerance" ) };
}

SApplyResult SAssertRecordedClipAction::apply( SProject *project )
{
    if( !project ) return { false, nullptr };
    SObject *mixer = splacements::rootContainer( project );
    SObject *lane  = splacements::laneAt( mixer, trackPath_ );
    if( !lane ) {
        qWarning() << "assert-recorded-clip: no lane at" << pathToString( trackPath_ );
        return { false, nullptr };
    }

    // Every non-container child with a duration is a clip on this lane.
    std::vector<SLink *> clips;
    for( int i = 0; i < lane->childCount(); ++i ) {
        SLink *lk = lane->childAt( i );
        if( !lk || lk->getSObject().isPathContainer() ) continue;
        if( !lk->getSObject().hasDuration() ) continue;
        clips.push_back( lk );
    }

    if( clips_ != kUnset && (qint64) clips.size() != clips_ ) {
        qWarning() << "assert-recorded-clip: lane holds" << (int) clips.size()
                   << "clip(s), expected" << clips_;
        return { false, nullptr };
    }
    if( clips.empty() ) {
        if( clips_ == 0 ) return { true, nullptr };
        qWarning() << "assert-recorded-clip: no clip on the lane";
        return { false, nullptr };
    }

    SLink   *link = clips.front();
    SObject &obj  = link->getSObject();

    if( takes_ != kUnset ) {
        STakeStack *stack = dynamic_cast<STakeStack *>( &obj );
        const qint64 n = stack ? stack->nTakes() : 1;
        if( n != takes_ ) {
            qWarning() << "assert-recorded-clip: column holds" << n
                       << "take(s), expected" << takes_;
            return { false, nullptr };
        }
    }

    if( growing_ && !obj.isLiveRecording() ) {
        qWarning() << "assert-recorded-clip: the clip is not a live recording";
        return { false, nullptr };
    }

    const qint64 at  = (qint64) link->getStartTime();
    const qint64 dur = (qint64) obj.getDurationBlocking();

    if( startFrame_ != kUnset && qAbs( at - startFrame_ ) > startTolerance_ ) {
        qWarning() << "assert-recorded-clip: clip starts at" << at
                   << "expected" << startFrame_ << "+-" << startTolerance_;
        return { false, nullptr };
    }
    if( durationFrames_ != kUnset
        && qAbs( dur - durationFrames_ ) > durationTolerance_ ) {
        qWarning() << "assert-recorded-clip: clip duration" << dur
                   << "expected" << durationFrames_ << "+-" << durationTolerance_;
        return { false, nullptr };
    }
    if( minDurationFrames_ != kUnset && dur < minDurationFrames_ ) {
        qWarning() << "assert-recorded-clip: clip duration" << dur
                   << "below the minimum" << minDurationFrames_;
        return { false, nullptr };
    }

    if( previewNonEmpty_ ) {
        // A WAVEFORM WHILE RECORDING. The probe array is what the arranger
        // draws, so a flat one means the growing clip is an empty box.
        const offset_t nProbes = 64;
        std::vector<preview_t> probes( (std::size_t) nProbes );
        for( auto &p : probes ) { p.min = 0; p.max = 0; }
        const int rc = obj.getPreview( probes.data(), 0,
                                       dur > 0 ? (length_t) dur : 1, nProbes );
        bool any = false;
        for( const preview_t &p : probes )
            if( p.min != 0 || p.max != 0 ) { any = true; break; }
        if( rc != 0 || !any ) {
            qWarning() << "assert-recorded-clip: preview is empty (rc" << rc
                       << ") over" << dur << "frames";
            return { false, nullptr };
        }
    }

    // ---- the placement conversion's own terms ------------------------------
    SAudioRecorder *rec = SApplication::app().audioRecorder();
    if( !rec
        && ( inputLatency_ != kUnset || outputLatency_ != kUnset
             || userOffset_ != kUnsetS || compensation_ != kUnsetS
             || trimmed_ != kUnset || passes_ != kUnset
             || sourceAtStart_ != kUnset ) ) {
        qWarning() << "assert-recorded-clip: no recorder to read the placement from";
        return { false, nullptr };
    }
    if( rec ) {
        const SRecordPlacement &pl = rec->placement();

        if( inputLatency_ != kUnset && pl.inputLatencyProj != inputLatency_ ) {
            qWarning() << "assert-recorded-clip: input latency"
                       << (qint64) pl.inputLatencyProj << "expected" << inputLatency_;
            return { false, nullptr };
        }
        if( outputLatency_ != kUnset && pl.outputLatencyProj != outputLatency_ ) {
            qWarning() << "assert-recorded-clip: output latency"
                       << (qint64) pl.outputLatencyProj << "expected" << outputLatency_;
            return { false, nullptr };
        }
        if( userOffset_ != kUnsetS && pl.userOffsetProj != userOffset_ ) {
            qWarning() << "assert-recorded-clip: user offset"
                       << (qint64) pl.userOffsetProj << "expected" << userOffset_;
            return { false, nullptr };
        }
        if( compensation_ != kUnsetS
            && pl.compensationFrames() != compensation_ ) {
            qWarning() << "assert-recorded-clip: compensation"
                       << (qint64) pl.compensationFrames()
                       << "expected" << compensation_;
            return { false, nullptr };
        }
        if( trimmed_ != kUnset && rec->trimmedFrames() != trimmed_ ) {
            qWarning() << "assert-recorded-clip: trimmed"
                       << (qint64) rec->trimmedFrames() << "expected" << trimmed_;
            return { false, nullptr };
        }
        if( passes_ != kUnset && rec->lastPassCount() != (int) passes_ ) {
            qWarning() << "assert-recorded-clip: committed" << rec->lastPassCount()
                       << "pass(es), expected" << passes_;
            return { false, nullptr };
        }

        // THE IDENTITY, always checked once the placement is anchored and the
        // take is over: what the recorder SAYS it did and where the clip
        // actually landed must be the same arithmetic. This is what makes the
        // decomposition above worth asserting -- a term could otherwise be
        // reported correctly and never applied.
        if( pl.anchored && !rec->isActive() && !clips.empty() && passes_ != 0 ) {
            const qint64 expect = (qint64) pl.placementFrame( rec->trimmedFrames() );
            const qint64 first  = (qint64) clips.front()->getStartTime();
            // Cycling and punching place at the region, not at the mapping's
            // own frame, so the identity is only claimed for a plain take.
            if( rec->lastPassCount() == 1 && startFrame_ == kUnset
                && expect >= 0 && first != expect ) {
                qWarning() << "assert-recorded-clip: placement identity broken —"
                           << "clip at" << first << "but the conversion says"
                           << expect << "(" << rec->describe() << ")";
                return { false, nullptr };
            }
        }

        // ---- FAITHFULNESS: the position-encoded source at the clip start ---
        if( sourceAtStart_ != kUnset ) {
            if( rec->lastFiles().isEmpty() ) {
                qWarning() << "assert-recorded-clip: the take wrote no file";
                return { false, nullptr };
            }
            const QString file = rec->lastFiles().first();
            if( !QFileInfo( file ).exists() ) {
                qWarning() << "assert-recorded-clip: recorded file missing:" << file;
                return { false, nullptr };
            }
            SClipWindow *win = SClipWindow::of( obj );
            const qint64 srcOff = win ? (qint64) win->startOffset() : 0;
            std::string err;
            const audio::PositionDecode d = audio::decodePositionAt(
                file.toStdString(), (std::uint64_t) srcOff, 4096, 0, err );
            if( d.silent || d.sourceFrame < 0 ) {
                qWarning() << "assert-recorded-clip: could not decode a source "
                              "position at clip offset" << srcOff
                           << QString::fromStdString( err );
                return { false, nullptr };
            }
            if( qAbs( (qint64) d.sourceFrame - sourceAtStart_ ) > sourceTolerance_ ) {
                qWarning() << "assert-recorded-clip: source position at the clip "
                              "start decoded as" << (qint64) d.sourceFrame
                           << "expected" << sourceAtStart_
                           << "+-" << sourceTolerance_
                           << "(clip srcOffset" << srcOff << ")";
                return { false, nullptr };
            }
            qDebug() << "assert-recorded-clip: source at start"
                     << (qint64) d.sourceFrame << "srcOffset" << srcOff
                     << "confidence" << d.confidence;
        }

        qDebug() << "assert-recorded-clip: OK —" << rec->describe()
                 << "clip at" << at << "dur" << dur
                 << "clips" << (int) clips.size();
    }

    return { true, nullptr };
}

void SAssertRecordedClipAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "trackPath", pathToString( trackPath_ ) );
    if( clips_ != kUnset )   elem.setAttribute( "clips", QString::number( clips_ ) );
    if( takes_ != kUnset )   elem.setAttribute( "takes", QString::number( takes_ ) );
    if( startFrame_ != kUnset )
        elem.setAttribute( "startFrame", QString::number( startFrame_ ) );
    if( startTolerance_ != 1024 )
        elem.setAttribute( "startTolerance", QString::number( startTolerance_ ) );
    if( durationFrames_ != kUnset )
        elem.setAttribute( "durationFrames", QString::number( durationFrames_ ) );
    if( durationTolerance_ != 4096 )
        elem.setAttribute( "durationTolerance", QString::number( durationTolerance_ ) );
    if( minDurationFrames_ != kUnset )
        elem.setAttribute( "minDurationFrames", QString::number( minDurationFrames_ ) );
    if( inputLatency_ != kUnset )
        elem.setAttribute( "inputLatencyFrames", QString::number( inputLatency_ ) );
    if( outputLatency_ != kUnset )
        elem.setAttribute( "outputLatencyFrames", QString::number( outputLatency_ ) );
    if( userOffset_ != kUnsetS )
        elem.setAttribute( "userOffsetFrames", QString::number( userOffset_ ) );
    if( compensation_ != kUnsetS )
        elem.setAttribute( "compensationFrames", QString::number( compensation_ ) );
    if( trimmed_ != kUnset )
        elem.setAttribute( "trimmedFrames", QString::number( trimmed_ ) );
    if( passes_ != kUnset )
        elem.setAttribute( "passes", QString::number( passes_ ) );
    if( growing_ )         elem.setAttribute( "growing", "true" );
    if( previewNonEmpty_ ) elem.setAttribute( "previewNonEmpty", "true" );
    if( sourceAtStart_ != kUnset )
        elem.setAttribute( "sourceAtStartFrame", QString::number( sourceAtStart_ ) );
    if( sourceTolerance_ != 4096 )
        elem.setAttribute( "sourceTolerance", QString::number( sourceTolerance_ ) );
}

bool SAssertRecordedClipAction::readXml( const QDomElement &elem, int )
{
    trackPath_ = stringToPath( elem.attribute( "trackPath" ) );
    clips_             = elem.attribute( "clips", "-1" ).toLongLong();
    takes_             = elem.attribute( "takes", "-1" ).toLongLong();
    startFrame_        = elem.attribute( "startFrame", "-1" ).toLongLong();
    startTolerance_    = elem.attribute( "startTolerance", "1024" ).toLongLong();
    durationFrames_    = elem.attribute( "durationFrames", "-1" ).toLongLong();
    durationTolerance_ = elem.attribute( "durationTolerance", "4096" ).toLongLong();
    minDurationFrames_ = elem.attribute( "minDurationFrames", "-1" ).toLongLong();
    inputLatency_      = elem.attribute( "inputLatencyFrames", "-1" ).toLongLong();
    outputLatency_     = elem.attribute( "outputLatencyFrames", "-1" ).toLongLong();
    userOffset_        = elem.attribute( "userOffsetFrames", "-999999" ).toLongLong();
    compensation_      = elem.attribute( "compensationFrames", "-999999" ).toLongLong();
    trimmed_           = elem.attribute( "trimmedFrames", "-1" ).toLongLong();
    passes_            = elem.attribute( "passes", "-1" ).toLongLong();
    growing_           = elem.attribute( "growing", "false" ) == "true";
    previewNonEmpty_   = elem.attribute( "previewNonEmpty", "false" ) == "true";
    sourceAtStart_     = elem.attribute( "sourceAtStartFrame", "-1" ).toLongLong();
    sourceTolerance_   = elem.attribute( "sourceTolerance", "4096" ).toLongLong();
    return true;
}

static const bool s_reg_record_verbs = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "record-start" ), []{ return new SRecordStartAction; } ),
    SActionRegistry::instance().registerType(
        QStringLiteral( "record-stop" ), []{ return new SRecordStopAction; } ),
    SActionRegistry::instance().registerType(
        QStringLiteral( "assert-recorded-clip" ),
        []{ return new SAssertRecordedClipAction; } ),
    true );
