#include "app/objects/sampler/ssamplerrecordactions.h"

#include "app/actions/sactionregistry.h"
#include "app/actions/scompositeaction.h"
#include "app/model/slink.h"
#include "app/model/sproject.h"
#include "app/objects/sampler/ssamplerlibraryactions.h"
#include "app/objects/sampler/ssamplerprogram.h"

#include "tw/sampler/twsamplerprogrammodel.h"

#include <QDebug>
#include <QDomDocument>
#include <QDomElement>

#include <string>
#include <vector>

using audio::twSamplerProgramModel;
using audio::twSamplerScope;

namespace {

using Attrs = QList<QPair<QString, QString>>;

// A verb built from its own XML form: through the REGISTRY when this module
// may not name it (create-file-asset), and for this module's own verbs too, so
// the plan is spelled in the grammar a script would write.
SAction *makeByName( const QString &name, const QString &pathRoot, const Attrs &attrs,
                     const QList<Attrs> &children = {}, const QString &childTag = QString() )
{
    SAction *a = SActionRegistry::instance().create( name );
    if( !a ) {
        qWarning() << "place-sampler-recording: no verb named" << name << "is registered";
        return nullptr;
    }
    QDomDocument doc;
    QDomElement  el = doc.createElement( name );
    for( const auto &kv : attrs ) el.setAttribute( kv.first, kv.second );
    for( const Attrs &c : children ) {
        QDomElement ce = doc.createElement( childTag );
        for( const auto &kv : c ) ce.setAttribute( kv.first, kv.second );
        el.appendChild( ce );
    }
    doc.appendChild( el );
    a->setPathRoot( pathRoot );
    a->readXml( el, a->formatVersion() );
    return a;
}

}  // namespace

// ======================================================= set-sampler-view ===

SApplyResult SSetSamplerViewAction::apply( SProject *project )
{
    QString why;
    if( view_.isEmpty() && padBank_ < 0 && pad_ < 0 )
        return refuse( QStringLiteral( "nothing to set (view=, padBank= or pad=)" ) );
    SSamplerProgram::View v = SSamplerProgram::View::Key;
    if( !view_.isEmpty() && !SSamplerProgram::viewFromText( view_, v ) )
        return refuse( QStringLiteral( "view=\"%1\" is not key, pad, map or zone" ).arg( view_ ) );

    // Validate against the state the program WILL have (or the defaults of
    // one not yet made) before the object is created.
    SSamplerProgram *prog = program( project, false, nullptr );
    SSamplerProgram::View oldView = prog ? prog->view() : SSamplerProgram::View::Key;
    const int oldBank = prog ? prog->padBank() : 0;
    const int oldPad  = prog ? prog->pad() : 0;
    SSamplerProgram::View newView = view_.isEmpty() ? oldView : v;
    const int newBank = padBank_ >= 0 ? padBank_ : oldBank;
    const int newPad  = pad_ >= 0 ? pad_ : oldPad;
    // A pad EXISTS when the program gives it a key: its <Pads> row, else its
    // default 36 + n (QBX-277). Only a pad being CHOSEN is checked, so a view
    // switch is never refused over the pad already selected.
    const audio::twSamplerProgramModel emptyModel;
    const audio::twSamplerProgramModel &m = prog ? prog->model() : emptyModel;
    const int newN = SSamplerProgram::padNumberOf( newBank, newPad );
    if( ( padBank_ >= 0 || pad_ >= 0 ) &&
        ( !SSamplerProgram::padInRange( newBank, newPad ) || m.padKey( newN ) < 0 ) )
        return refuse( QStringLiteral( "pad %1 of bank %2 does not exist (banks 0..%3 of %4 pads; "
                                       "a pad exists when the program gives it a key, its <Pads> "
                                       "row or its default %5 + n, at most 127)" )
                           .arg( newPad ).arg( newBank ).arg( SSamplerProgram::kPadBanks - 1 )
                           .arg( SSamplerProgram::kPadsPerBank )
                           .arg( SSamplerProgram::kPadDefaultBaseKey ) );
    if( !prog ) {
        prog = program( project, true, &why );
        if( !prog ) return refuse( why );
        // A program object made NOW starts in the per-user default view
        // (Q7.1, QBX-279: the resolver sets it), which is the view this verb
        // keeps when it names none and the one its inverse restores.
        oldView = prog->view();
        if( view_.isEmpty() ) newView = oldView;
    }

    prog->setViewState( newView, newBank, newPad );
    qInfo().noquote() << QStringLiteral( "set-sampler-view: track %1 slot %2: view %3, bank %4, "
                                         "pad %5 (key %6)" )
                             .arg( trackPath_ ).arg( slotIndex_ )
                             .arg( SSamplerProgram::viewText( newView ) ).arg( newBank )
                             .arg( newPad ).arg( prog->padKey() );
    return { true, new SSetSamplerViewAction( trackPath_, slotIndex_,
                                              SSamplerProgram::viewText( oldView ), oldBank,
                                              oldPad ) };
}

void SSetSamplerViewAction::writeXml( QDomElement &elem ) const
{
    writeAddress( elem );
    if( !view_.isEmpty() ) elem.setAttribute( "view", view_ );
    if( padBank_ >= 0 ) elem.setAttribute( "padBank", padBank_ );
    if( pad_ >= 0 ) elem.setAttribute( "pad", pad_ );
}

bool SSetSamplerViewAction::readXml( const QDomElement &elem, int /*version*/ )
{
    readAddress( elem );
    view_ = elem.attribute( "view" );
    bool ok = true;
    padBank_ = elem.hasAttribute( "padBank" ) ? elem.attribute( "padBank" ).toInt( &ok ) : -1;
    if( !ok ) padBank_ = 1 << 20;   // not a number: refused by apply as a pad that does not exist
    ok = true;
    pad_ = elem.hasAttribute( "pad" ) ? elem.attribute( "pad" ).toInt( &ok ) : -1;
    if( !ok ) pad_ = 1 << 20;
    return true;
}

QStringList SSetSamplerViewAction::knownAttributes() const
{
    return addressAttributes() << QStringLiteral( "view" ) << QStringLiteral( "padBank" )
                               << QStringLiteral( "pad" );
}

// ============================================================ set-pad-key ===

SApplyResult SSetPadKeyAction::apply( SProject *project )
{
    QString why;
    if( !hasN_ || !hasKey_ ) return refuse( QStringLiteral( "set-pad-key needs n= and key=" ) );
    // A LIBRARY program (DESIGN §8; the author's decision of 2026-11-04,
    // QBX-278): the reassignment is an OVERLAY edit, the core's set-pad with
    // key.was, never a change to the read-only base. The inverse puts the
    // instance's program state back as it was, which removes the edit and
    // writes the previous value back.
    if( SSamplerProgram *lib = program( project, false, nullptr ); lib && lib->isLibrary() ) {
        const SSamplerProgram::ProgramState before = lib->programState();
        int prior = -1;
        std::string err;
        if( !lib->editLibrary( [&]( audio::twSamplerLibrary &l ) { return l.recordPad( n_, key_, &prior, &err ); },
                               &why ) )
            return refuse( err.empty() ? why : QString::fromStdString( err ) );
        qInfo().noquote() << QStringLiteral( "set-pad-key: track %1 slot %2: pad %3 (bank %4, pad %5) "
                                             "plays key %6 (was %7), an overlay edit on the library "
                                             "\"%8\"" )
                                 .arg( trackPath_ ).arg( slotIndex_ ).arg( n_ )
                                 .arg( n_ / SSamplerProgram::kPadsPerBank )
                                 .arg( n_ % SSamplerProgram::kPadsPerBank ).arg( key_ ).arg( prior )
                                 .arg( QString::fromStdString( lib->library()->basePath() ) );
        return { true, new SRestoreSamplerStateAction( trackPath_, slotIndex_, before.library, before.program,
                                                       before.base, before.overlay ) };
    }
    // The core validates n (0..95) and key (0..127; -1 only for a pad past the
    // defaults): refused on a scratch copy first, so a refused edit never
    // creates the program object (invariant 4).
    {
        audio::twSamplerProgramModel scratch;
        SSamplerProgram *existing = program( project, false, nullptr );
        if( existing ) scratch = existing->model();
        std::string err;
        if( !scratch.setPadKey( n_, key_, nullptr, &err ) )
            return refuse( QString::fromStdString( err ) );
    }
    SSamplerProgram *prog = program( project, true, &why );
    if( !prog ) return refuse( why );
    int prior = -1;
    std::string err;
    if( !prog->edit( [&]( audio::twSamplerProgramModel &m ) { return m.setPadKey( n_, key_, &prior, &err ); },
                     &why ) )
        return refuse( err.empty() ? why : QString::fromStdString( err ) );
    qInfo().noquote() << QStringLiteral( "set-pad-key: track %1 slot %2: pad %3 (bank %4, pad %5) "
                                         "plays key %6 (was %7)" )
                             .arg( trackPath_ ).arg( slotIndex_ ).arg( n_ )
                             .arg( n_ / SSamplerProgram::kPadsPerBank )
                             .arg( n_ % SSamplerProgram::kPadsPerBank ).arg( key_ ).arg( prior );
    return { true, new SSetPadKeyAction( trackPath_, slotIndex_, n_, prior ) };
}

void SSetPadKeyAction::writeXml( QDomElement &elem ) const
{
    writeAddress( elem );
    if( hasN_ ) elem.setAttribute( "n", n_ );
    if( hasKey_ ) elem.setAttribute( "key", key_ );
}

bool SSetPadKeyAction::readXml( const QDomElement &elem, int /*version*/ )
{
    readAddress( elem );
    bool ok = true;
    hasN_ = elem.hasAttribute( "n" );
    n_    = hasN_ ? elem.attribute( "n" ).toInt( &ok ) : -1;
    if( !ok ) n_ = 1 << 20;   // not a number: refused by the core as a pad that does not exist
    ok      = true;
    hasKey_ = elem.hasAttribute( "key" );
    key_    = hasKey_ ? elem.attribute( "key" ).toInt( &ok ) : -1;
    if( !ok ) key_ = 1 << 20;
    return true;
}

QStringList SSetPadKeyAction::knownAttributes() const
{
    return addressAttributes() << QStringLiteral( "n" ) << QStringLiteral( "key" );
}

// =============================================== set-sampler-record-input ===

SApplyResult SSetSamplerRecordInputAction::apply( SProject *project )
{
    QString why;
    if( !hasInput_ ) return refuse( QStringLiteral( "no input= (audio:<device>:<mask>, or \"\" for none)" ) );
    const QString spec = input_ == QLatin1String( "none" ) ? QString() : input_;
    if( !SSamplerProgram::isValidRecordInput( spec ) )
        return refuse( QStringLiteral( "input=\"%1\" is not audio:<device>:<mask> with a non-zero "
                                       "hex mask, or empty" ).arg( input_ ) );
    SSamplerProgram *prog = program( project, true, &why );
    if( !prog ) return refuse( why );
    const QString before = prog->recordInput();
    prog->setRecordInput( spec );
    qInfo().noquote() << QStringLiteral( "set-sampler-record-input: track %1 slot %2: \"%3\"" )
                             .arg( trackPath_ ).arg( slotIndex_ ).arg( spec );
    return { true, new SSetSamplerRecordInputAction( trackPath_, slotIndex_, before ) };
}

void SSetSamplerRecordInputAction::writeXml( QDomElement &elem ) const
{
    writeAddress( elem );
    elem.setAttribute( "input", input_ );
}

bool SSetSamplerRecordInputAction::readXml( const QDomElement &elem, int /*version*/ )
{
    readAddress( elem );
    hasInput_ = elem.hasAttribute( "input" );
    input_    = elem.attribute( "input" );
    return true;
}

QStringList SSetSamplerRecordInputAction::knownAttributes() const
{
    return addressAttributes() << QStringLiteral( "input" );
}

// ================================================ place-sampler-recording ===

SPlaceSamplerRecordingAction::~SPlaceSamplerRecordingAction()
{
    delete plan_;
}

SApplyResult SPlaceSamplerRecordingAction::apply( SProject *project )
{
    QString why;

    // THE REDO: the same children again, which put back the same zone and
    // layer ids and register the same asset name.
    if( plan_ ) {
        SApplyResult r = applyPropagatingRoot( plan_, project );
        if( !r.applied ) return refuse( QStringLiteral( "the redo could not be re-applied" ) );
        return { true, r.inverse };
    }

    if( !project ) return { false, nullptr };
    if( filePath_.isEmpty() ) return refuse( QStringLiteral( "no filePath" ) );
    if( srcOffset_ < 0 || length_ < 0 )
        return refuse( QStringLiteral( "srcOffset and length must not be negative" ) );
    if( !target_.isEmpty() && target_ != QLatin1String( "pad" ) && target_ != QLatin1String( "key" ) )
        return refuse( QStringLiteral( "target=\"%1\" is not pad or key" ).arg( target_ ) );
    if( key_ > 127 ) return refuse( QStringLiteral( "key=%1 is not a MIDI key" ).arg( key_ ) );

    // PRE-FLIGHT the file before anything changes: the program child would
    // otherwise be applied and rolled back, and the program OBJECT created for
    // nothing (invariant 4).
    {
        QString path = filePath_;
        SLink *probe = project->linkToFile( path );
        const length_t len = ( probe && probe->getSObject().hasDuration() )
                                 ? probe->getSObject().getDurationBlocking() : 0;
        delete probe;
        if( len <= 0 )
            return refuse( QStringLiteral( "\"%1\" does not open as audio" ).arg( filePath_ ) );
        if( srcOffset_ >= (qint64)len )
            return refuse( QStringLiteral( "srcOffset %1 is at or past the recording's %2 frame(s)" )
                               .arg( srcOffset_ ).arg( (qlonglong)len ) );
    }

    SSamplerProgram *prog = program( project, false, nullptr );
    if( prog && !prog->isReadable() )
        return refuse( QStringLiteral( "the stored program could not be read (%1)" ).arg( prog->loadError() ) );
    const bool padView = prog && prog->view() == SSamplerProgram::View::Pad;
    const QString target = !target_.isEmpty() ? target_
                                              : ( padView ? QStringLiteral( "pad" ) : QStringLiteral( "key" ) );

    Attrs layer{ { QStringLiteral( "src" ), QStringLiteral( "file:" ) + filePath_ },
                 { QStringLiteral( "start" ), QString::number( srcOffset_ ) } };
    if( length_ > 0 ) layer.append( { QStringLiteral( "end" ), QString::number( srcOffset_ + length_ ) } );

    const Attrs addr{ { QStringLiteral( "trackPath" ), trackPath_ },
                      { QStringLiteral( "slotIndex" ), QString::number( slotIndex_ ) } };
    SAction *programChild = nullptr;
    QString  landing;
    int      key = -1;
    if( target == QLatin1String( "pad" ) ) {
        // The pad's key comes from the program's <Pads> table (QBX-277).
        if( key_ >= 0 ) key = key_;
        else if( prog ) key = prog->padKey();
        else key = audio::twSamplerProgramModel().padKey( 0 );
        if( key < 0 )
            return refuse( QStringLiteral( "pad %1 has no key (its default would pass 127 and the "
                                           "program's <Pads> table gives it none)" )
                               .arg( prog ? prog->padNumber() : 0 ) );
        // THE PAD'S ZONE IS THE ZONE COVERING ITS KEY (DESIGN §13, settled
        // 2026-11-03, pads): several pads may share it, and all of them hear
        // the take. More than one zone covering the key has no one zone to
        // record into, so the take is refused rather than guessed.
        std::uint32_t padZone = 0;
        if( prog ) {
            const std::vector<std::uint32_t> covering = prog->model().zonesCoveringKey( key );
            if( covering.size() > 1 ) {
                QStringList ids;
                for( std::uint32_t z : covering ) ids << QString::number( z );
                return refuse( QStringLiteral( "key %1 is covered by %2 zones (%3: velocity layers "
                                               "or round-robin zones), so the pad has no one zone "
                                               "to record into; nothing changed" )
                                   .arg( key ).arg( covering.size() ).arg( ids.join( QStringLiteral( ", " ) ) ) );
            }
            if( covering.size() == 1 ) padZone = covering.front();
        }
        if( padZone != 0 ) {
            const std::vector<std::uint32_t> layers = prog->model().layerIds( padZone );
            if( !layers.empty() ) {
                // REPLACE layer 1 (DESIGN §7): the sample and its positions;
                // the loop points belonged to the old sample and are reset, as
                // is its length in beats. Every other setting is kept. A
                // negative ABSOLUTE value is spelled with `=` (the front end's
                // value syntax).
                Attrs set = addr;
                set.append( { QStringLiteral( "layer" ), QString::number( layers.front() ) } );
                set.append( layer );
                if( length_ <= 0 ) set.append( { QStringLiteral( "end" ), QStringLiteral( "=-1" ) } );
                set.append( { QStringLiteral( "loopStart" ), QStringLiteral( "=-1" ) } );
                set.append( { QStringLiteral( "loopEnd" ), QStringLiteral( "=-1" ) } );
                set.append( { QStringLiteral( "beats" ), QStringLiteral( "0" ) } );
                programChild = makeByName( QStringLiteral( "set-layer" ), pathRoot(), set );
                landing = QStringLiteral( "pad key %1: layer %2 of zone %3 replaced" )
                              .arg( key ).arg( layers.front() ).arg( padZone );
            } else {
                Attrs add = addr;
                add.append( { QStringLiteral( "zone" ), QString::number( padZone ) } );
                add.append( layer );
                programChild = makeByName( QStringLiteral( "add-layer" ), pathRoot(), add );
                landing = QStringLiteral( "pad key %1: a layer added to zone %2" ).arg( key ).arg( padZone );
            }
        } else {
            Attrs zone = addr;
            zone.append( { QStringLiteral( "keyLo" ), QString::number( key ) } );
            zone.append( { QStringLiteral( "keyHi" ), QString::number( key ) } );
            zone.append( { QStringLiteral( "rootKey" ), QString::number( key ) } );
            programChild = makeByName( QStringLiteral( "add-zone" ), pathRoot(), zone, { layer },
                                       QStringLiteral( "layer" ) );
            landing = QStringLiteral( "pad key %1: a new one-key zone" ).arg( key );
        }
    } else if( !prog || prog->model().zoneCount() == 0 ) {
        Attrs zone = addr;
        zone.append( { QStringLiteral( "keyLo" ), QStringLiteral( "0" ) } );
        zone.append( { QStringLiteral( "keyHi" ), QStringLiteral( "127" ) } );
        programChild = makeByName( QStringLiteral( "add-zone" ), pathRoot(), zone, { layer },
                                   QStringLiteral( "layer" ) );
        landing = QStringLiteral( "key view, empty program: a zone over the whole keyboard" );
    } else {
        key = key_ >= 0 ? key_ : prog->lastKey();
        if( key < 0 )
            return refuse( QStringLiteral( "the program has zones and no key has been played on "
                                           "the instance's MIDI input yet, so there is no last key "
                                           "played to land on (DESIGN §7)" ) );
        Attrs zone = addr;
        zone.append( { QStringLiteral( "keyLo" ), QString::number( key ) } );
        zone.append( { QStringLiteral( "keyHi" ), QString::number( key ) } );
        zone.append( { QStringLiteral( "rootKey" ), QString::number( key ) } );
        programChild = makeByName( QStringLiteral( "add-zone" ), pathRoot(), zone, { layer },
                                   QStringLiteral( "layer" ) );
        landing = QStringLiteral( "key view: a new one-key zone on the last key played, %1" ).arg( key );
    }
    if( !programChild ) return refuse( QStringLiteral( "the program verb could not be built" ) );

    // Q-4: the recording is ALSO a registered qbx asset.
    SAction *assetChild = makeByName( QStringLiteral( "create-file-asset" ), pathRoot(),
                                      { { QStringLiteral( "filePath" ), filePath_ },
                                        { QStringLiteral( "srcOffset" ), QString::number( srcOffset_ ) },
                                        { QStringLiteral( "length" ), QString::number( length_ ) } } );
    if( !assetChild ) {
        delete programChild;
        return refuse( QStringLiteral( "create-file-asset is not registered" ) );
    }

    plan_ = new SCompositeAction( QList<SAction *>{ programChild, assetChild } );
    plan_->setPathRoot( pathRoot() );
    SApplyResult r = applyPropagatingRoot( plan_, project );
    if( !r.applied ) {
        delete plan_;
        plan_ = nullptr;
        return refuse( QStringLiteral( "landing it failed (%1); nothing changed" ).arg( landing ) );
    }
    qInfo().noquote() << QStringLiteral( "place-sampler-recording: track %1 slot %2: %3 <- %4" )
                             .arg( trackPath_ ).arg( slotIndex_ ).arg( landing, filePath_ );
    return { true, r.inverse };
}

void SPlaceSamplerRecordingAction::writeXml( QDomElement &elem ) const
{
    writeAddress( elem );
    elem.setAttribute( "filePath", filePath_ );
    elem.setAttribute( "srcOffset", srcOffset_ );
    elem.setAttribute( "length", length_ );
    if( !target_.isEmpty() ) elem.setAttribute( "target", target_ );
    if( key_ >= 0 ) elem.setAttribute( "key", key_ );
}

bool SPlaceSamplerRecordingAction::readXml( const QDomElement &elem, int /*version*/ )
{
    readAddress( elem );
    filePath_  = elem.attribute( "filePath" );
    srcOffset_ = elem.attribute( "srcOffset", "0" ).toLongLong();
    length_    = elem.attribute( "length", "0" ).toLongLong();
    target_    = elem.attribute( "target" );
    key_       = elem.attribute( "key", "-1" ).toInt();
    return true;
}

QStringList SPlaceSamplerRecordingAction::knownAttributes() const
{
    return addressAttributes() << QStringLiteral( "filePath" ) << QStringLiteral( "srcOffset" )
                               << QStringLiteral( "length" ) << QStringLiteral( "target" )
                               << QStringLiteral( "key" );
}

static const bool s_reg_sampler_record_verbs = [] {
    SActionRegistry &r = SActionRegistry::instance();
    r.registerType( QStringLiteral( "set-sampler-view" ), [] { return new SSetSamplerViewAction; } );
    r.registerType( QStringLiteral( "set-pad-key" ), [] { return new SSetPadKeyAction; } );
    r.registerType( QStringLiteral( "set-sampler-record-input" ),
                    [] { return new SSetSamplerRecordInputAction; } );
    r.registerType( QStringLiteral( "place-sampler-recording" ),
                    [] { return new SPlaceSamplerRecordingAction; } );
    return true;
}();
