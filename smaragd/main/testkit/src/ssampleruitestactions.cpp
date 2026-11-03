#include "app/testkit/ssampleruitestactions.h"

#include "app/actions/sactionregistry.h"
#include "app/model/splacements.h"
#include "app/model/sproject.h"
#include "app/objects/track/spluginchain.h"
#include "app/objects/track/spluginslot.h"
#include "app/objects/track/strack.h"
#include "app/pluginui/spluginappeditors.h"
#include "app/samplerui/ssamplereditor.h"
#include "app/shell/smainwindow.h"
#include "app/testkit/smidiintestactions.h"

#include "tw/devices/capture_midi.h"
#include "tw/devices/midi_out_scheduler.h"

#include <QApplication>
#include <QDebug>
#include <QDomElement>
#include <QStringList>

namespace {

struct Address {
    STrack      *track = nullptr;
    SPluginSlot *slot  = nullptr;
};

Address resolve( SProject *project, const QString &root, const QString &trackPath, int slotIndex,
                 const QString &verb )
{
    Address a;
    QString r = root;
    a.track = dynamic_cast<STrack *>( splacements::laneBySpec( project, trackPath, r ) );
    if( !a.track ) {
        qWarning().noquote() << verb << ": no track at" << trackPath;
        return a;
    }
    SPluginChain *chain = a.track->getPluginChain();
    a.slot = chain ? chain->getSlotAt( slotIndex ) : nullptr;
    if( !a.slot ) qWarning().noquote() << verb << ": track" << trackPath << "has no slot" << slotIndex;
    return a;
}

QWidget *mainWindow()
{
    for( QWidget *w : QApplication::topLevelWidgets() )
        if( SMainWindow *win = qobject_cast<SMainWindow *>( w ) ) return win;
    return nullptr;
}

QList<int> parseKeys( const QString &text, bool *ok )
{
    QList<int> out;
    *ok = true;
    for( const QString &part : text.split( QLatin1Char( ';' ), Qt::SkipEmptyParts ) ) {
        bool good = false;
        const int k = part.trimmed().toInt( &good );
        if( !good || k < 0 || k > 127 ) {
            *ok = false;
            return {};
        }
        out << k;
    }
    return out;
}

}  // namespace

// ============================================================ sampler-editor ==

QStringList SSamplerEditorAction::knownAttributes() const
{
    return { QStringLiteral( "trackPath" ), QStringLiteral( "slotIndex" ), QStringLiteral( "action" ),
             QStringLiteral( "view" ),      QStringLiteral( "bank" ),      QStringLiteral( "pad" ),
             QStringLiteral( "key" ),       QStringLiteral( "vel" ),       QStringLiteral( "knob" ),
             QStringLiteral( "values" ),    QStringLiteral( "press" ),     QStringLiteral( "text" ),
             QStringLiteral( "cursor" ),    QStringLiteral( "choice" ),    QStringLiteral( "payload" ),
             QStringLiteral( "target" ),    QStringLiteral( "expect" ) };
}

SApplyResult SSamplerEditorAction::apply( SProject *project )
{
    const Address a = resolve( project, pathRoot_, trackPath_, slotIndex_, name() );
    if( !a.slot ) return { false, nullptr };

    bool ok = false;
    if( action_ == QLatin1String( "open" ) ) {
        // Through the REGISTRY the FX strip consults, parented to the main
        // window as the strip's window() would be; nothing is shown.
        ok = spluginappeditors::openFor( a.track, a.slot, mainWindow(), /*showWindow=*/false );
    } else {
        SSamplerEditor *ed = SSamplerEditor::editorFor( a.slot );
        if( !ed ) {
            qWarning().noquote() << "sampler-editor: REJECTED - no editor is open on track"
                                 << trackPath_ << "slot" << slotIndex_;
            return { false, nullptr };
        }
        if( action_ == QLatin1String( "close" ) ) {
            SSamplerEditor::closeFor( a.slot );
            ok = !SSamplerEditor::isOpenFor( a.slot );
        } else if( action_ == QLatin1String( "view" ) ) {
            ok = ed->clickView( view_ ) && ed->view() == [this]() {
                SSamplerProgram::View v = SSamplerProgram::View::Key;
                SSamplerProgram::viewFromText( view_, v );
                return v;
            }();
        } else if( action_ == QLatin1String( "bank" ) ) {
            ok = ed->clickBank( bank_ );
        } else if( action_ == QLatin1String( "pad" ) ) {
            ok = ed->clickPad( pad_ );
        } else if( action_ == QLatin1String( "key" ) ) {
            ok = ed->clickKey( key_ );
        } else if( action_ == QLatin1String( "map" ) ) {
            ok = ed->clickMap( key_, vel_ );
        } else if( action_ == QLatin1String( "knob" ) ) {
            ok = true;
            if( press_ ) ok = ed->knobPress( knob_ );
            for( const QString &v : values_.split( QLatin1Char( ';' ), Qt::SkipEmptyParts ) ) {
                bool good = false;
                const double d = v.trimmed().toDouble( &good );   // C locale
                if( !good ) {
                    qWarning().noquote() << "sampler-editor: values" << values_ << "is not a list of numbers";
                    return { false, nullptr };
                }
                ok = ed->knobSet( knob_, d ) && ok;
            }
            if( press_ ) ok = ed->knobRelease( knob_ ) && ok;
        } else if( action_ == QLatin1String( "xml-type" ) ) {
            ok = ed->xmlType( text_, cursor_ );
        } else if( action_ == QLatin1String( "xml-accept" ) ) {
            ok = ed->xmlAcceptCompletion( choice_ );
        } else if( action_ == QLatin1String( "xml-apply" ) ) {
            ok = ed->xmlApply();
        } else if( action_ == QLatin1String( "drop" ) ) {
            ok = ed->dropResource( payload_, target_ );
        } else {
            qWarning().noquote() << "sampler-editor: unknown action" << action_;
            return { false, nullptr };
        }
    }
    if( ok != ( expect_ != 0 ) ) {
        SSamplerEditor *ed = SSamplerEditor::editorFor( a.slot );
        qWarning().noquote() << "sampler-editor FAILED:" << action_ << "returned" << ok << "expected"
                             << ( expect_ != 0 ) << "|" << ( ed ? ed->describe() : QString() );
        return { false, nullptr };
    }
    return { true, nullptr };
}

void SSamplerEditorAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "trackPath", trackPath_ );
    elem.setAttribute( "slotIndex", QString::number( slotIndex_ ) );
    elem.setAttribute( "action", action_ );
    if( !view_.isEmpty() ) elem.setAttribute( "view", view_ );
    if( bank_ >= 0 ) elem.setAttribute( "bank", QString::number( bank_ ) );
    if( pad_ >= 0 ) elem.setAttribute( "pad", QString::number( pad_ ) );
    if( key_ >= 0 ) elem.setAttribute( "key", QString::number( key_ ) );
    if( vel_ >= 0 ) elem.setAttribute( "vel", QString::number( vel_ ) );
    if( !knob_.isEmpty() ) elem.setAttribute( "knob", knob_ );
    if( !values_.isEmpty() ) elem.setAttribute( "values", values_ );
    elem.setAttribute( "press", QString::number( press_ ) );
    if( !text_.isEmpty() ) elem.setAttribute( "text", text_ );
    elem.setAttribute( "cursor", QString::number( cursor_ ) );
    if( !choice_.isEmpty() ) elem.setAttribute( "choice", choice_ );
    if( !payload_.isEmpty() ) elem.setAttribute( "payload", payload_ );
    if( !target_.isEmpty() ) elem.setAttribute( "target", target_ );
    elem.setAttribute( "expect", QString::number( expect_ ) );
}

bool SSamplerEditorAction::readXml( const QDomElement &elem, int )
{
    trackPath_ = elem.attribute( "trackPath", "0" );
    slotIndex_ = elem.attribute( "slotIndex", "0" ).toInt();
    action_    = elem.attribute( "action" );
    view_      = elem.attribute( "view" );
    bank_      = elem.attribute( "bank", "-1" ).toInt();
    pad_       = elem.attribute( "pad", "-1" ).toInt();
    key_       = elem.attribute( "key", "-1" ).toInt();
    vel_       = elem.attribute( "vel", "-1" ).toInt();
    knob_      = elem.attribute( "knob" );
    values_    = elem.attribute( "values" );
    press_     = elem.attribute( "press", "1" ).toInt();
    text_      = elem.attribute( "text" );
    cursor_    = elem.attribute( "cursor", "-1" ).toInt();
    choice_    = elem.attribute( "choice" );
    payload_   = elem.attribute( "payload" );
    target_    = elem.attribute( "target" );
    expect_    = elem.attribute( "expect", "1" ).toInt();
    return true;
}

// ===================================================== assert-sampler-editor ==

QStringList SAssertSamplerEditorAction::knownAttributes() const
{
    return { QStringLiteral( "trackPath" ), QStringLiteral( "slotIndex" ), QStringLiteral( "open" ),
             QStringLiteral( "flag" ),      QStringLiteral( "contains" ),  QStringLiteral( "absent" ) };
}

SApplyResult SAssertSamplerEditorAction::apply( SProject *project )
{
    if( open_ < 0 && flag_ < 0 && contains_.isEmpty() && absent_.isEmpty() ) {
        qWarning() << "assert-sampler-editor: REJECTED - the element checks nothing";
        return { false, nullptr };
    }
    const Address a = resolve( project, pathRoot_, trackPath_, slotIndex_, name() );
    if( !a.slot ) return { false, nullptr };
    bool pass = true;
    const bool isOpen = spluginappeditors::isOpenFor( a.slot );
    if( open_ >= 0 && isOpen != ( open_ != 0 ) ) {
        qWarning() << "assert-sampler-editor FAILED: open is" << isOpen << "expected" << ( open_ != 0 );
        pass = false;
    }
    if( flag_ >= 0 && a.slot->getEditorOpen() != ( flag_ != 0 ) ) {
        qWarning() << "assert-sampler-editor FAILED: editorOpen flag is" << a.slot->getEditorOpen()
                   << "expected" << ( flag_ != 0 );
        pass = false;
    }
    if( !contains_.isEmpty() || !absent_.isEmpty() ) {
        SSamplerEditor *ed = SSamplerEditor::editorFor( a.slot );
        if( !ed ) {
            qWarning() << "assert-sampler-editor FAILED: contains/absent need an open editor";
            return { false, nullptr };
        }
        const QString d = ed->describe();
        if( !contains_.isEmpty() && !d.contains( contains_ ) ) {
            qWarning().noquote() << "assert-sampler-editor FAILED: no" << contains_ << "in" << d;
            pass = false;
        }
        if( !absent_.isEmpty() && d.contains( absent_ ) ) {
            qWarning().noquote() << "assert-sampler-editor FAILED:" << absent_ << "is in" << d;
            pass = false;
        }
    }
    return { pass, nullptr };
}

void SAssertSamplerEditorAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "trackPath", trackPath_ );
    elem.setAttribute( "slotIndex", QString::number( slotIndex_ ) );
    if( open_ >= 0 ) elem.setAttribute( "open", QString::number( open_ ) );
    if( flag_ >= 0 ) elem.setAttribute( "flag", QString::number( flag_ ) );
    if( !contains_.isEmpty() ) elem.setAttribute( "contains", contains_ );
    if( !absent_.isEmpty() ) elem.setAttribute( "absent", absent_ );
}

bool SAssertSamplerEditorAction::readXml( const QDomElement &elem, int )
{
    trackPath_ = elem.attribute( "trackPath", "0" );
    slotIndex_ = elem.attribute( "slotIndex", "0" ).toInt();
    open_      = elem.attribute( "open", "-1" ).toInt();
    flag_      = elem.attribute( "flag", "-1" ).toInt();
    contains_  = elem.attribute( "contains" );
    absent_    = elem.attribute( "absent" );
    return true;
}

// ========================================================= sampler-hold-keys ==

QStringList SSamplerHoldKeysAction::knownAttributes() const
{
    return { QStringLiteral( "trackPath" ), QStringLiteral( "slotIndex" ), QStringLiteral( "keys" ),
             QStringLiteral( "release" ),   QStringLiteral( "velocity" ),  QStringLiteral( "channel" ) };
}

SApplyResult SSamplerHoldKeysAction::apply( SProject *project )
{
    const Address a = resolve( project, pathRoot_, trackPath_, slotIndex_, name() );
    if( !a.slot ) return { false, nullptr };
    SSamplerEditor *ed = SSamplerEditor::editorFor( a.slot );
    if( !ed ) {
        qWarning().noquote() << "sampler-hold-keys: REJECTED - no editor is open on track"
                             << trackPath_ << "slot" << slotIndex_
                             << "(the hand-selection tap exists only while it is)";
        return { false, nullptr };
    }
    bool okKeys = false, okRel = false;
    const QList<int> keys = parseKeys( keys_, &okKeys );
    const QList<int> rel  = parseKeys( release_, &okRel );
    if( !okKeys || !okRel || ( keys.isEmpty() && rel.isEmpty() ) || channel_ < 0 || channel_ > 15 ||
        velocity_ < 0 || velocity_ > 127 ) {
        qWarning().noquote() << "sampler-hold-keys: REJECTED - keys" << keys_ << "release" << release_
                             << "velocity" << velocity_ << "channel" << channel_;
        return { false, nullptr };
    }
    QString why;
    audio::CaptureMidiInput *port = smidiin::injectionPort( why );
    if( !port ) {
        qWarning().noquote() << "sampler-hold-keys: REJECTED -" << why;
        return { false, nullptr };
    }
    auto send = [&]( quint8 status, int key, int vel ) {
        const quint8 msg[3] = { (quint8)( status | ( channel_ & 0x0F ) ), (quint8) key, (quint8) vel };
        const qint64 at = audio::MidiOutScheduler::hostNowNs();
        port->inject( msg, 3, at );
        smidiin::setLastInjectedHostNs( at );
    };
    for( int k : keys ) send( 0x90, k, velocity_ );
    for( int k : rel ) send( 0x80, k, 0 );
    ed->pollTap();
    return { true, nullptr };
}

void SSamplerHoldKeysAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "trackPath", trackPath_ );
    elem.setAttribute( "slotIndex", QString::number( slotIndex_ ) );
    elem.setAttribute( "keys", keys_ );
    elem.setAttribute( "release", release_ );
    elem.setAttribute( "velocity", QString::number( velocity_ ) );
    elem.setAttribute( "channel", QString::number( channel_ ) );
}

bool SSamplerHoldKeysAction::readXml( const QDomElement &elem, int )
{
    trackPath_ = elem.attribute( "trackPath", "0" );
    slotIndex_ = elem.attribute( "slotIndex", "0" ).toInt();
    keys_      = elem.attribute( "keys" );
    release_   = elem.attribute( "release" );
    velocity_  = elem.attribute( "velocity", "100" ).toInt();
    channel_   = elem.attribute( "channel", "0" ).toInt();
    return true;
}

static const bool s_reg_sampler_ui_verbs = [] {
    SActionRegistry &r = SActionRegistry::instance();
    r.registerType( QStringLiteral( "sampler-editor" ), [] { return new SSamplerEditorAction; } );
    r.registerType( QStringLiteral( "assert-sampler-editor" ),
                    [] { return new SAssertSamplerEditorAction; } );
    r.registerType( QStringLiteral( "sampler-hold-keys" ), [] { return new SSamplerHoldKeysAction; } );
    return true;
}();
