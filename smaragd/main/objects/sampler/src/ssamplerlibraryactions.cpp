#include "app/objects/sampler/ssamplerlibraryactions.h"

#include "app/actions/sactionregistry.h"
#include "app/objects/sampler/ssamplerprogram.h"

#include "tw/sampler/twsamplerprogrammodel.h"

#include <QDebug>
#include <QDomElement>
#include <QFile>

#include <string>

namespace {

// The inverse that puts `state` back on the instance at this address.
SAction *restoreOf( const QString &trackPath, int slotIndex, const SSamplerProgram::ProgramState &s )
{
    return new SRestoreSamplerStateAction( trackPath, slotIndex, s.library, s.program, s.base,
                                           s.overlay );
}

QString reportText( const audio::twSamplerLibrary &lib )
{
    const std::string r = lib.report();
    return r.empty() ? QStringLiteral( "no drift" ) : QString::fromStdString( r );
}

}  // namespace

// =================================================== set-sampler-library ===

SApplyResult SSetSamplerLibraryAction::apply( SProject *project )
{
    QString why;
    if( path_.isEmpty() ) return refuse( QStringLiteral( "set-sampler-library needs path=" ) );
    if( !read_ ) {
        QFile f( path_ );
        if( !f.open( QIODevice::ReadOnly ) )
            return refuse( QStringLiteral( "the library \"%1\" cannot be read" ).arg( path_ ) );
        baseText_ = f.readAll().toStdString();
    }
    // Checked on a scratch copy first, so a refusal never creates the program
    // object (invariant 4).
    {
        audio::twSamplerLibrary scratch;
        std::string err;
        if( !audio::twSamplerLibrary::fromXml( baseText_, std::string(), scratch, &err ) )
            return refuse( QString::fromStdString( err ) );
    }
    read_ = true;
    SSamplerProgram *prog = program( project, true, &why );
    if( !prog ) return refuse( why );
    const SSamplerProgram::ProgramState before = prog->programState();
    SSamplerProgram::ProgramState next;
    next.library = true;
    next.base    = baseText_;
    if( prog->isLibrary() ) {
        next.overlay = prog->library()->overlayXml();   // the overlay is the user's: kept
    }
    {
        // The overlay names where its base came from.
        audio::twSamplerLibrary lib;
        std::string err;
        if( !audio::twSamplerLibrary::fromXml( next.base, next.overlay, lib, &err ) )
            return refuse( QString::fromStdString( err ) );
        lib.setBasePath( path_.toStdString() );
        next.base    = lib.baseXml();
        next.overlay = lib.overlayXml();
    }
    if( !prog->restoreProgramState( next, &why ) ) return refuse( why );
    qInfo().noquote() << QStringLiteral( "set-sampler-library: track %1 slot %2: plays the library "
                                         "\"%3\" (version \"%4\") with %5 overlay edit(s): %6" )
                             .arg( trackPath_ ).arg( slotIndex_ ).arg( path_ )
                             .arg( QString::fromStdString( prog->library()->libraryVersion() ) )
                             .arg( prog->library()->editCount() )
                             .arg( reportText( *prog->library() ) );
    return { true, restoreOf( trackPath_, slotIndex_, before ) };
}

void SSetSamplerLibraryAction::writeXml( QDomElement &elem ) const
{
    writeAddress( elem );
    elem.setAttribute( "path", path_ );
}

bool SSetSamplerLibraryAction::readXml( const QDomElement &elem, int /*version*/ )
{
    readAddress( elem );
    path_ = elem.attribute( "path" );
    read_ = false;
    return true;
}

QStringList SSetSamplerLibraryAction::knownAttributes() const
{
    return addressAttributes() << QStringLiteral( "path" );
}

// ================================================ rebase-sampler-library ===

SApplyResult SRebaseSamplerLibraryAction::apply( SProject *project )
{
    QString why;
    SSamplerProgram *prog = program( project, false, &why );
    if( !prog ) return refuse( why );
    if( !prog->isLibrary() ) return refuse( QStringLiteral( "this instance does not play a library program" ) );
    const SSamplerProgram::ProgramState before = prog->programState();
    const QString reportBefore = reportText( *prog->library() );
    int refreshed = 0;
    if( !prog->editLibrary( [&]( audio::twSamplerLibrary &lib ) {
                                refreshed = lib.rebase();
                                return true;
                            },
                            &why ) )
        return refuse( why );
    qInfo().noquote() << QStringLiteral( "rebase-sampler-library: track %1 slot %2: %3 edit(s) "
                                         "refreshed (was: %4; now: %5)" )
                             .arg( trackPath_ ).arg( slotIndex_ ).arg( refreshed )
                             .arg( reportBefore, reportText( *prog->library() ) );
    return { true, restoreOf( trackPath_, slotIndex_, before ) };
}

void SRebaseSamplerLibraryAction::writeXml( QDomElement &elem ) const { writeAddress( elem ); }

bool SRebaseSamplerLibraryAction::readXml( const QDomElement &elem, int /*version*/ )
{
    readAddress( elem );
    return true;
}

QStringList SRebaseSamplerLibraryAction::knownAttributes() const { return addressAttributes(); }

// ============================================================== hide-pad ===

SApplyResult SHidePadAction::apply( SProject *project )
{
    QString why;
    if( !hasN_ ) return refuse( QStringLiteral( "hide-pad needs n=" ) );
    SSamplerProgram *prog = program( project, false, &why );
    if( !prog ) return refuse( why );
    if( !prog->isLibrary() )
        return refuse( QStringLiteral( "this instance does not play a library program; hide-pad hides a "
                                       "library's pad row (use set-pad-key on a program of its own)" ) );
    const SSamplerProgram::ProgramState before = prog->programState();
    const int keyBefore = prog->model().padKey( n_ );
    std::string err;
    if( !prog->editLibrary( [&]( audio::twSamplerLibrary &lib ) { return lib.hidePad( n_, &err ); }, &why ) )
        return refuse( err.empty() ? why : QString::fromStdString( err ) );
    qInfo().noquote() << QStringLiteral( "hide-pad: track %1 slot %2: pad %3's library row (key %4) is "
                                         "hidden; the pad plays key %5" )
                             .arg( trackPath_ ).arg( slotIndex_ ).arg( n_ ).arg( keyBefore )
                             .arg( prog->model().padKey( n_ ) );
    return { true, restoreOf( trackPath_, slotIndex_, before ) };
}

void SHidePadAction::writeXml( QDomElement &elem ) const
{
    writeAddress( elem );
    if( hasN_ ) elem.setAttribute( "n", n_ );
}

bool SHidePadAction::readXml( const QDomElement &elem, int /*version*/ )
{
    readAddress( elem );
    bool ok = true;
    hasN_ = elem.hasAttribute( "n" );
    n_    = hasN_ ? elem.attribute( "n" ).toInt( &ok ) : -1;
    if( !ok ) n_ = 1 << 20;   // not a number: refused by the core as a pad that does not exist
    return true;
}

QStringList SHidePadAction::knownAttributes() const
{
    return addressAttributes() << QStringLiteral( "n" );
}

// ================================================ flatten-sampler-library ===

SApplyResult SFlattenSamplerLibraryAction::apply( SProject *project )
{
    QString why;
    SSamplerProgram *prog = program( project, false, &why );
    if( !prog ) return refuse( why );
    if( !prog->isLibrary() )
        return refuse( QStringLiteral( "this instance does not play a library program; there is nothing "
                                       "to flatten" ) );
    const SSamplerProgram::ProgramState before = prog->programState();
    const QString basePath = QString::fromStdString( prog->library()->basePath() );
    std::string dropped;
    SSamplerProgram::ProgramState next;
    next.library = false;
    next.program = prog->library()->flattened( &dropped ).toXml();
    if( !prog->restoreProgramState( next, &why ) ) return refuse( why );
    qInfo().noquote() << QStringLiteral( "flatten-sampler-library: track %1 slot %2: the library \"%3\" is "
                                         "now a program of its own, %4 zone(s); dropped: %5" )
                             .arg( trackPath_ ).arg( slotIndex_ ).arg( basePath )
                             .arg( prog->model().zoneCount() )
                             .arg( dropped.empty() ? QStringLiteral( "none" ) : QString::fromStdString( dropped ) );
    return { true, restoreOf( trackPath_, slotIndex_, before ) };
}

void SFlattenSamplerLibraryAction::writeXml( QDomElement &elem ) const { writeAddress( elem ); }

bool SFlattenSamplerLibraryAction::readXml( const QDomElement &elem, int /*version*/ )
{
    readAddress( elem );
    return true;
}

QStringList SFlattenSamplerLibraryAction::knownAttributes() const { return addressAttributes(); }

// ================================================= restore-sampler-state ===

SApplyResult SRestoreSamplerStateAction::apply( SProject *project )
{
    QString why;
    SSamplerProgram *prog = program( project, false, &why );
    if( !prog ) return refuse( why );
    const SSamplerProgram::ProgramState before = prog->programState();
    SSamplerProgram::ProgramState s;
    s.library = library_;
    s.program = program_;
    s.base    = base_;
    s.overlay = overlay_;
    if( !prog->restoreProgramState( s, &why ) ) return refuse( why );
    qInfo().noquote() << QStringLiteral( "restore-sampler-state: track %1 slot %2: %3" )
                             .arg( trackPath_ ).arg( slotIndex_ )
                             .arg( library_ ? QStringLiteral( "a library program, %1 overlay edit(s)" )
                                                  .arg( prog->library()->editCount() )
                                            : QStringLiteral( "a program of its own" ) );
    return { true, restoreOf( trackPath_, slotIndex_, before ) };
}

void SRestoreSamplerStateAction::writeXml( QDomElement &elem ) const
{
    writeAddress( elem );
    elem.setAttribute( "library", library_ ? 1 : 0 );
    if( library_ ) {
        elem.setAttribute( "base", QString::fromStdString( base_ ) );
        elem.setAttribute( "overlay", QString::fromStdString( overlay_ ) );
    } else {
        elem.setAttribute( "program", QString::fromStdString( program_ ) );
    }
}

bool SRestoreSamplerStateAction::readXml( const QDomElement &elem, int /*version*/ )
{
    readAddress( elem );
    library_ = elem.attribute( "library", "0" ) == QLatin1String( "1" );
    program_ = elem.attribute( "program" ).toStdString();
    base_    = elem.attribute( "base" ).toStdString();
    overlay_ = elem.attribute( "overlay" ).toStdString();
    return true;
}

QStringList SRestoreSamplerStateAction::knownAttributes() const
{
    return addressAttributes() << QStringLiteral( "library" ) << QStringLiteral( "program" )
                               << QStringLiteral( "base" ) << QStringLiteral( "overlay" );
}

static const bool s_reg_sampler_library_verbs = [] {
    SActionRegistry &r = SActionRegistry::instance();
    r.registerType( QStringLiteral( "set-sampler-library" ), [] { return new SSetSamplerLibraryAction; } );
    r.registerType( QStringLiteral( "rebase-sampler-library" ),
                    [] { return new SRebaseSamplerLibraryAction; } );
    r.registerType( QStringLiteral( "hide-pad" ), [] { return new SHidePadAction; } );
    r.registerType( QStringLiteral( "flatten-sampler-library" ),
                    [] { return new SFlattenSamplerLibraryAction; } );
    r.registerType( QStringLiteral( "restore-sampler-state" ),
                    [] { return new SRestoreSamplerStateAction; } );
    return true;
}();
