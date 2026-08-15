#include "app/objects/midi/smidifileactions.h"

#include <QDebug>
#include <QDir>
#include <QDomDocument>
#include <QDomElement>
#include <QFileInfo>
#include <string>
#include <vector>

#include "app/actions/sactionregistry.h"
#include "app/actions/scompositeaction.h"
#include "app/model/sclipwindow.h"
#include "app/model/sfilepathref.h"
#include "app/model/slink.h"
#include "app/model/sobjectpath.h"
#include "app/model/splacements.h"
#include "app/model/sproject.h"
#include "app/objects/midi/smidiactionsupport.h"
#include "app/objects/midi/smidiclipactions.h"
#include "app/objects/midi/smidicut.h"
#include "app/objects/midi/smidisequence.h"
#include "tw/events/twsmf.h"
#include "tw/events/twtempomap.h"

using namespace strackpath;

namespace {

/** twEvent -> SEvent. The reverse of smidievents::buildSeq at identity scale. */
std::vector<SEvent> fromEngine( const twEventSeq &seq )
{
    std::vector<SEvent> out;
    out.reserve( seq.size() );
    for( const twEvent &e : seq.events() ) {
        SEvent m;
        m.t = e.time;
        m.dur = e.duration;
        m.kind = e.kind;
        m.channel = e.channel;
        m.key = e.key;
        m.paramId = e.paramId;
        m.value = e.value;
        m.value2 = e.value2;
        if( e.payloadSize ) {
            const uint8_t *p = seq.payload( e );
            if( p ) m.blob = QByteArray( (const char *) p, (int) e.payloadSize );
        }
        out.push_back( m );
    }
    return out;
}

/** Resolve an input path the way linkToFile does (script base dir first). */
QString resolveInputPath( SProject *project, const QString &given )
{
    if( !project ) return given;
    if( QFileInfo( given ).isAbsolute() ) return given;
    const QString base = project->sampleBaseDir();
    if( !base.isEmpty() ) {
        const QString resolved =
            QDir::cleanPath( QDir( base ).filePath( given ) );
        if( QFileInfo::exists( resolved ) ) return resolved;
    }
    // Next to the project file, then the cwd - the same order SFilePathRef
    // resolves a stored reference in.
    if( !project->projectFilePath().isEmpty() ) {
        const QString anchored = SFilePathRef::fromStored(
            given, project->projectFilePath() );
        if( QFileInfo::exists( anchored ) ) return anchored;
    }
    return given;
}

/** Does the project have any placed material yet? Decides the tempo policy. */
bool projectIsEmpty( SProject *project )
{
    SObject *root = splacements::rootContainer( project );
    if( !root ) return true;
    for( SLink *lk : root->childLinks() ) {
        if( !lk ) continue;
        SObject &obj = lk->getSObject();
        if( !obj.isPathContainer() ) return false;   // a clip on the root
        if( obj.childCount() > 0 ) return false;     // a track with something on it
    }
    return true;
}

/** Ensure `lane index` exists directly under the root mixer; returns the lane. */
SObject *ensureLane( SProject *project, const QList<int> &basePath, int extra )
{
    SObject *mixer = splacements::rootContainer( project );
    QList<int> path = basePath;
    if( path.isEmpty() ) path.append( 0 );
    path.last() += extra;
    if( SObject *lane = splacements::laneAt( mixer, path ) ) return lane;

    // Build one through the registry rather than by naming SAddTrackAction:
    // objects/midi sits at the rank of objects/cut and has no edge to
    // objects/mixer, and "add a track" is a verb, not a type.
    QDomDocument doc;
    QDomElement el = doc.createElement( "add-track" );
    el.setAttribute( "index", "-1" );
    doc.appendChild( el );
    SAction *add = SActionRegistry::instance().create( QStringLiteral( "add-track" ) );
    if( !add ) return nullptr;
    add->readXml( el, add->formatVersion() );
    SApplyResult r = add->apply( project );
    delete add;
    delete r.inverse;
    if( !r.applied ) return nullptr;
    return splacements::laneAt( mixer, path );
}

}  // namespace

// ---------------------------------------------------------------------------
// import-midi-file
// ---------------------------------------------------------------------------

SImportMidiFileAction::SImportMidiFileAction( const QList<int> &trackPath,
                                              const QString &filePath,
                                              offset_t timePos,
                                              const QString &mode,
                                              bool newTracks )
    : trackPath_( trackPath ), filePath_( filePath ), timePos_( timePos ),
      mode_( mode ), newTracks_( newTracks )
{
}

SApplyResult SImportMidiFileAction::apply( SProject *project )
{
    if( !project || filePath_.isEmpty() ) {
        qWarning() << "import-midi-file: filePath is required";
        return { false, nullptr };
    }
    const QString path = resolveInputPath( project, filePath_ );
    twSmfFile file;
    std::string err;
    if( !twSmf::readFile( path.toStdString(), file, &err ) ) {
        qWarning() << "import-midi-file:" << path << "-"
                   << QString::fromStdString( err );
        return { false, nullptr };
    }
    // Everything in this app is PPQ 960 (D2). The rescale is exact for a file
    // whose ticks are multiples of oldPpq/gcd, which every authored file is.
    if( file.ppq != SMidiSequence::DEFAULT_PPQ )
        file = twSmf::rescalePpq( file, SMidiSequence::DEFAULT_PPQ );

    // Tempo policy: the FIRST tempo meta re-tempos an empty project and only
    // warns otherwise. It stays in the sequence either way - dropping it would
    // make an export write a different file than the import read.
    bool warnedSegments = false;
    int tempoSeen = 0;
    double firstBpm = 0.0;
    for( const twSmfTrack &tr : file.tracks ) {
        if( !tr.events ) continue;
        for( const twEvent &e : tr.events->events() ) {
            if( e.kind != twEventKind::Tempo ) continue;
            ++tempoSeen;
            if( tempoSeen == 1 && e.value > 0.0 ) firstBpm = 6.0e7 / e.value;
        }
    }
    if( tempoSeen > 1 && !warnedSegments ) {
        qWarning() << "import-midi-file:" << path << "carries" << tempoSeen
                   << "tempo events; timing here is CONSTANT until tempo "
                      "segments land (proposal 37). They are kept as Tempo "
                      "metadata events and exported unchanged.";
        warnedSegments = true;
    }

    SCompositeAction *composite = new SCompositeAction;
    const bool merged = ( mode_.compare( "merged", Qt::CaseInsensitive ) == 0 );

    if( firstBpm > 0.0 ) {
        if( projectIsEmpty( project ) ) {
            composite->append( new SSetTempoAction( firstBpm ) );
        } else if( qAbs( firstBpm - project->getBPMTempo() ) > 1e-9 ) {
            qWarning() << "import-midi-file:" << path << "declares"
                       << firstBpm << "BPM but the project already holds"
                       << project->getBPMTempo()
                       << "BPM and is not empty; the project tempo is kept.";
        }
    }

    // Build the model events per SMF track first, so an unreadable file costs
    // nothing and the placement loop is trivial.
    struct Imported { QString name; std::vector<SEvent> events; qint64 endTick; };
    std::vector<Imported> imported;
    for( const twSmfTrack &tr : file.tracks ) {
        Imported im;
        im.name = QString::fromStdString( tr.name );
        im.endTick = tr.endTick;
        if( tr.events ) im.events = fromEngine( *tr.events );
        imported.push_back( std::move( im ) );
    }
    if( merged && imported.size() > 1 ) {
        Imported all;
        all.name = imported.front().name;
        all.endTick = 0;
        for( const Imported &im : imported ) {
            all.events.insert( all.events.end(), im.events.begin(), im.events.end() );
            all.endTick = qMax( all.endTick, im.endTick );
        }
        imported.clear();
        imported.push_back( std::move( all ) );
    }

    for( size_t i = 0; i < imported.size(); ++i ) {
        SObject *lane = newTracks_
            ? ensureLane( project, trackPath_, (int) i )
            : splacements::laneAt( splacements::rootContainer( project ),
                                   trackPath_ );
        if( !lane ) {
            qWarning() << "import-midi-file: no lane for SMF track" << (int) i;
            delete composite;
            return { false, nullptr };
        }
        QList<int> lanePath = strackpath::pathOf(
            splacements::rootContainer( project ), lane );
        composite->append( new SInsertMidiClipAction(
            lanePath, timePos_, imported[i].endTick, imported[i].name,
            imported[i].events ) );
    }

    if( composite->count() == 0 ) { delete composite; return { false, nullptr }; }
    SApplyResult r = composite->apply( project );
    delete composite;
    return r;
}

void SImportMidiFileAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "trackPath", pathToString( trackPath_ ) );
    elem.setAttribute( "filePath", filePath_ );
    elem.setAttribute( "timePos", QString::fromStdString(
                           Fraction( (int64_t) timePos_, 1 ).toString() ) );
    elem.setAttribute( "mode", mode_ );
    elem.setAttribute( "newTracks", newTracks_ ? 1 : 0 );
}

bool SImportMidiFileAction::readXml( const QDomElement &elem, int )
{
    trackPath_ = stringToPath( elem.attribute( "trackPath" ) );
    filePath_ = elem.attribute( "filePath", "" );
    timePos_ = (offset_t) parseFractionOrDouble(
        elem.attribute( "timePos", "0" ).toStdString() ).toDouble();
    mode_ = elem.attribute( "mode", "tracks" );
    newTracks_ = elem.attribute( "newTracks", "1" ).toInt() != 0;
    return true;
}

static const bool s_reg_import_midi_file = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "import-midi-file" ),
        []{ return new SImportMidiFileAction; } ), true );

// ---------------------------------------------------------------------------
// export-midi-file
// ---------------------------------------------------------------------------

SExportMidiFileAction::SExportMidiFileAction( const QString &filePath,
                                              const QList<int> &clipPath,
                                              const QList<int> &trackPath,
                                              int type )
    : filePath_( filePath ), clipPath_( clipPath ), trackPath_( trackPath ),
      hasClip_( !clipPath.isEmpty() ), hasTrack_( !trackPath.isEmpty() ),
      type_( type )
{
}

namespace {

void collectFromLane( SObject *lane, twSmfFile &out )
{
    if( !lane ) return;
    for( SLink *lk : lane->childLinks() ) {
        if( !lk ) continue;
        SObject &obj = lk->getSObject();
        if( obj.isPathContainer() ) { collectFromLane( &obj, out ); continue; }
        SMidiCut *cut = dynamic_cast<SMidiCut *>( &obj );
        if( !cut ) continue;
        SMidiSequence *seq = cut->sequence();
        if( !seq ) continue;
        twSmfTrack tr;
        tr.name = seq->getSName().toStdString();
        tr.events = seq->tickSnapshot();
        tr.endTick = seq->lengthTicks();
        out.tracks.push_back( std::move( tr ) );
    }
}

}  // namespace

SApplyResult SExportMidiFileAction::apply( SProject *project )
{
    if( !project || filePath_.isEmpty() ) {
        qWarning() << "export-midi-file: filePath is required";
        return { false, nullptr };
    }
    twSmfFile file;
    file.format = type_;
    file.ppq = SMidiSequence::DEFAULT_PPQ;

    SObject *mixer = splacements::rootContainer( project );
    if( hasClip_ ) {
        smidiactions::ClipRef ref =
            smidiactions::resolveClip( project, clipPath_ );
        if( !ref.valid() ) {
            qWarning() << "export-midi-file: no MIDI clip at"
                       << pathToString( clipPath_ );
            return { false, nullptr };
        }
        twSmfTrack tr;
        tr.name = ref.seq->getSName().toStdString();
        tr.events = ref.seq->tickSnapshot();
        tr.endTick = ref.seq->lengthTicks();
        file.tracks.push_back( std::move( tr ) );
    } else if( hasTrack_ ) {
        collectFromLane( splacements::laneAt( mixer, trackPath_ ), file );
    } else {
        // Whole project, in ARRANGEMENT ORDER - one SMF track per event clip,
        // which is exactly what import produced, so a round trip is identity.
        collectFromLane( mixer, file );
    }

    if( file.tracks.empty() ) {
        qWarning() << "export-midi-file: nothing to export";
        return { false, nullptr };
    }
    if( file.format == 0 && file.tracks.size() > 1 ) {
        qWarning() << "export-midi-file: type 0 requested but"
                   << (int) file.tracks.size() << "tracks were collected";
        return { false, nullptr };
    }

    QString path = filePath_;
    if( !QFileInfo( path ).isAbsolute() && !project->sampleBaseDir().isEmpty() )
        path = QDir::cleanPath( QDir( project->sampleBaseDir() ).filePath( path ) );

    std::string err;
    if( !twSmf::writeFile( path.toStdString(), file, &err ) ) {
        qWarning() << "export-midi-file:" << path << "-"
                   << QString::fromStdString( err );
        return { false, nullptr };
    }
    qDebug() << "export-midi-file: wrote" << path << "with"
             << (int) file.tracks.size() << "track(s)";
    return { true, nullptr };   // writing a file is not undoable
}

void SExportMidiFileAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "filePath", filePath_ );
    if( hasClip_ )  elem.setAttribute( "clip", pathToString( clipPath_ ) );
    if( hasTrack_ ) elem.setAttribute( "trackPath", pathToString( trackPath_ ) );
    elem.setAttribute( "type", type_ );
}

bool SExportMidiFileAction::readXml( const QDomElement &elem, int )
{
    filePath_ = elem.attribute( "filePath", "" );
    hasClip_ = elem.hasAttribute( "clip" );
    hasTrack_ = elem.hasAttribute( "trackPath" );
    clipPath_ = stringToPath( elem.attribute( "clip" ) );
    trackPath_ = stringToPath( elem.attribute( "trackPath" ) );
    type_ = elem.attribute( "type", "1" ).toInt();
    return true;
}

static const bool s_reg_export_midi_file = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "export-midi-file" ),
        []{ return new SExportMidiFileAction; } ), true );
