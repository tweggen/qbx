#include "app/samplerui/ssamplereditor.h"

#include "app/actions/saction.h"
#include "app/actions/sactionhistory.h"
#include "app/actions/sactionregistry.h"
#include "app/actions/scompositeaction.h"
#include "app/model/splacements.h"
#include "app/model/sproject.h"
#include "app/objects/track/spluginchain.h"
#include "app/objects/track/spluginslot.h"
#include "app/objects/track/strack.h"
#include "app/pluginui/spluginappeditors.h"
#include "app/samplerui/ssamplerviews.h"
#include "app/samplerui/ssamplerxmlpane.h"
#include "app/samplerui/ssamplerzones.h"
#include "app/shell/sapplication.h"
#include "app/shell/smidiinputhub.h"
#include "app/shell/ssamplerwiring.h"

#include "tw/core/twlog.h"
#include "tw/sampler/twsamplerprogrammodel.h"

#include <QButtonGroup>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QDial>
#include <QDomDocument>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QHBoxLayout>
#include <QHash>
#include <QLabel>
#include <QMimeData>
#include <QMouseEvent>
#include <QPushButton>
#include <QStackedWidget>
#include <QTimer>
#include <QUndoStack>
#include <QVBoxLayout>

#include <algorithm>
#include <memory>

namespace {

// MODULE-LEVEL, keyed by slot: the window outlives any strip (the native
// editor's registry, spluginnativeeditor.cpp, for the same reason).
QHash<SPluginSlot *, QPointer<SSamplerEditor>> &registry()
{
    static QHash<SPluginSlot *, QPointer<SSamplerEditor>> r;
    return r;
}

const SSamplerEditor::View kViews[4] = { SSamplerEditor::View::Key, SSamplerEditor::View::Pad,
                                         SSamplerEditor::View::Map, SSamplerEditor::View::Zone };

QString sizeText( const QSize &s )
{
    return QStringLiteral( "%1x%2" ).arg( s.width() ).arg( s.height() );
}

bool isFixed( const QWidget *w )
{
    return w && w->minimumSize() == w->maximumSize() && w->minimumSize().isValid();
}

QString idList( const QVector<std::uint32_t> &ids )
{
    QStringList parts;
    for( std::uint32_t id : ids ) parts << QString::number( id );
    return parts.join( QLatin1Char( ';' ) );
}

// A verb element built from its tag and attributes (the drop's verbs: a path
// with a quote in it must not be spliced into XML text).
QDomElement verbElement( QDomDocument &doc, const QString &tag,
                         const QList<QPair<QString, QString>> &attrs )
{
    QDomElement e = doc.createElement( tag );
    for( const auto &a : attrs ) e.setAttribute( a.first, a.second );
    return e;
}

}  // namespace

// ---------------------------------------------------------------- the registry

namespace {

// The static initializer that puts this window behind the FX strip's edit
// action (pluginui CONTRACT, spluginappeditors.h). Legal in an OBJECT library
// (docs/ARCHITECTURE.md, "OBJECT is load-bearing"): every object file is linked.
const bool s_registered = [] {
    spluginappeditors::Entry e;
    e.open = []( STrack *track, SPluginSlot *slot, QWidget *parent, bool show ) {
        return SSamplerEditor::openFor( track, slot, parent, show ) != nullptr;
    };
    e.isOpen = []( SPluginSlot *slot ) { return SSamplerEditor::isOpenFor( slot ); };
    e.close  = []( SPluginSlot *slot ) { SSamplerEditor::closeFor( slot ); };
    spluginappeditors::registerEditor( ssamplerwiring::nassauDigitalUid(), e );
    return true;
}();

}  // namespace

SSamplerEditor *SSamplerEditor::openFor( STrack *track, SPluginSlot *slot,
                                         QWidget *parentForPosition, bool showWindow )
{
    if( !track || !slot || !ssamplerwiring::slotHostsNassauDigital( slot ) ) return nullptr;
    SSamplerEditor *w = registry().value( slot ).data();
    if( !w ) {
        w = new SSamplerEditor( track, slot,
                                parentForPosition ? parentForPosition->window() : nullptr );
        registry().insert( slot, w );
        // The flag travels in the project (the native editor's rule, pluginui
        // CONTRACT): set BEFORE any show, so a headless open is right too.
        slot->setEditorOpen( true );
    }
    if( showWindow ) {
        w->show();
        w->raise();
        w->activateWindow();
    }
    return w;
}

SSamplerEditor *SSamplerEditor::editorFor( SPluginSlot *slot )
{
    return registry().value( slot ).data();
}

bool SSamplerEditor::isOpenFor( SPluginSlot *slot ) { return editorFor( slot ) != nullptr; }

void SSamplerEditor::closeFor( SPluginSlot *slot )
{
    if( SSamplerEditor *w = editorFor( slot ) ) w->close();
}

// ---------------------------------------------------------------- the window

QSize SSamplerEditor::keyViewSize() { return QSize( 128 * SSamplerKeyView::kKeyWidth, 144 ); }
QSize SSamplerEditor::padViewSize()
{
    return QSize( SSamplerPadView::kGap + 4 * ( SSamplerPadView::kCell + SSamplerPadView::kGap ),
                  SSamplerPadView::kGridTop + 4 * ( SSamplerPadView::kCell + SSamplerPadView::kGap ) );
}
QSize SSamplerEditor::mapViewSize()
{
    return QSize( SSamplerMapView::kLeft + 128 * SSamplerMapView::kKeyW + 8,
                  SSamplerMapView::kTop + 128 * SSamplerMapView::kVelH + 24 );
}
QSize SSamplerEditor::zoneViewSize() { return QSize( 760, 400 ); }

QString SSamplerEditor::viewName( View v ) { return SSamplerProgram::viewText( v ); }

SSamplerEditor::SSamplerEditor( STrack *track, SPluginSlot *slot, QWidget *parent )
    : QWidget( parent, Qt::Window ), track_( track ), slot_( slot )
{
    setAttribute( Qt::WA_DeleteOnClose );
    setObjectName( QStringLiteral( "samplerEditor" ) );

    auto *outer = new QVBoxLayout( this );
    outer->setContentsMargins( 8, 8, 8, 8 );
    outer->setSpacing( 6 );
    // The window takes the size its fixed view and the pane need, and nothing
    // else: DESIGN §7's views are fixed-size.
    outer->setSizeConstraint( QLayout::SetFixedSize );

    auto *bar = new QHBoxLayout;
    viewButtons_ = new QButtonGroup( this );
    viewButtons_->setExclusive( true );
    const char *const labels[4] = { QT_TR_NOOP( "Key" ), QT_TR_NOOP( "Pad" ), QT_TR_NOOP( "Map" ),
                                    QT_TR_NOOP( "Zone" ) };
    for( int i = 0; i < 4; ++i ) {
        auto *b = new QPushButton( tr( labels[i] ), this );
        b->setCheckable( true );
        b->setObjectName( QStringLiteral( "view-%1" ).arg( viewName( kViews[i] ) ) );
        viewButtons_->addButton( b, i );
        viewButton_[i] = b;
        bar->addWidget( b );
        connect( b, &QPushButton::clicked, this, [this, i]() {
            if( view() == kViews[i] && program() ) {
                showView_( kViews[i] );
                return;
            }
            QString why;
            if( !submitVerbXml( QStringLiteral( "<set-sampler-view view=\"%1\"/>" )
                                    .arg( viewName( kViews[i] ) ),
                                0, &why ) ) {
                setStatus( why );
                refresh();   // put the buttons back on the view that IS shown
            }
        } );
    }
    bar->addStretch( 1 );
    outer->addLayout( bar );

    stack_    = new QStackedWidget( this );
    keyView_  = new SSamplerKeyView( this, stack_ );
    padView_  = new SSamplerPadView( this, stack_ );
    mapView_  = new SSamplerMapView( this, stack_ );
    zoneView_ = new SSamplerZoneView( this, stack_ );
    stack_->addWidget( keyView_ );
    stack_->addWidget( padView_ );
    stack_->addWidget( mapView_ );
    stack_->addWidget( zoneView_ );
    outer->addWidget( stack_, 0, Qt::AlignLeft );

    xmlPane_ = new SSamplerXmlPane( this, this );
    outer->addWidget( xmlPane_ );

    status_ = new QLabel( this );
    status_->setObjectName( QStringLiteral( "status" ) );
    status_->setWordWrap( true );
    outer->addWidget( status_ );

    // The slot going away takes the window with it (slotDestroying, not
    // destroyed(): the native editor's reason, spluginslot.h).
    connect( slot, &SPluginSlot::slotDestroying, this, &QWidget::close );

    tapTimer_ = new QTimer( this );
    tapTimer_->setInterval( 10 );
    connect( tapTimer_, &QTimer::timeout, this, [this]() { pollTap(); } );
    connect( &SApplication::app(), &SApplication::liveLanesMayHaveChanged, this,
             &SSamplerEditor::syncTap );

    project_ = SApplication::app().getCurrentProject();
    if( project_ )
        connect( project_, &SProject::arrangementChanged, this, &SSamplerEditor::refresh );

    refresh();
}

SSamplerEditor::~SSamplerEditor()
{
    releaseTap_();
    for( auto it = registry().begin(); it != registry().end(); ) {
        if( it.value().isNull() || it.value().data() == this ) it = registry().erase( it );
        else ++it;
    }
}

void SSamplerEditor::closeEvent( QCloseEvent *e )
{
    // CLOSED MEANS DEAD, before the deferred delete: WA_DeleteOnClose deletes
    // through deleteLater, and a test-case run may never deliver it, so a
    // closed window that still heard arrangementChanged would re-acquire its
    // tap in refresh() and go on selecting zones from the keys (measured: it
    // did, until this flag).
    closed_ = true;
    if( project_ ) disconnect( project_, nullptr, this, nullptr );
    if( boundProgram_ ) disconnect( boundProgram_, nullptr, this, nullptr );
    disconnect( &SApplication::app(), nullptr, this, nullptr );
    releaseTap_();
    if( slot_ ) slot_->setEditorOpen( false );
    // Out of the registry NOW: WA_DeleteOnClose deletes later (deleteLater),
    // and a window on its way out is not open.
    for( auto it = registry().begin(); it != registry().end(); ) {
        if( it.value().data() == this ) it = registry().erase( it );
        else ++it;
    }
    QWidget::closeEvent( e );
}

SSamplerProgram *SSamplerEditor::program() const
{
    return ssamplerwiring::programOf( slot_.data() );
}

SSamplerEditor::View SSamplerEditor::view() const
{
    if( SSamplerProgram *p = program() ) return p->view();
    return static_cast<View>( ssamplerwiring::newInstanceView() );
}

QString SSamplerEditor::currentTrackPath() const
{
    SProject *project = SApplication::app().getCurrentProject();
    if( !track_ || !project ) return QString();
    // Qualified by the root that holds the track (QBX-231/246): empty when the
    // track has left the project.
    return splacements::specOf( project, track_ );
}

int SSamplerEditor::currentSlotIndex() const
{
    if( !track_ || !slot_ ) return -1;
    SPluginChain *chain = track_->getPluginChain();
    if( !chain ) return -1;
    for( int i = 0; i < chain->getSlotCount(); ++i )
        if( chain->getSlotAt( i ) == slot_.data() ) return i;
    return -1;
}

void SSamplerEditor::bindProgram_()
{
    SSamplerProgram *p = program();
    if( p == boundProgram_.data() ) return;
    if( boundProgram_ ) disconnect( boundProgram_, nullptr, this, nullptr );
    boundProgram_ = p;
    if( p ) {
        connect( p, &SSamplerProgram::programChanged, this, &SSamplerEditor::refresh );
        connect( p, &SSamplerProgram::selectionChanged, this, &SSamplerEditor::refresh );
        connect( p, &SSamplerProgram::viewChanged, this, &SSamplerEditor::refresh );
    }
}

QWidget *SSamplerEditor::pageFor_( View v ) const
{
    switch( v ) {
    case View::Key:  return keyView_;
    case View::Pad:  return padView_;
    case View::Map:  return mapView_;
    case View::Zone: return zoneView_;
    }
    return keyView_;
}

void SSamplerEditor::showView_( View v )
{
    QWidget *page = pageFor_( v );
    stack_->setCurrentWidget( page );
    // The stack takes the CURRENT page's fixed size, so the window is exactly
    // as large as the view shown (a QStackedWidget would otherwise size to its
    // largest page).
    stack_->setFixedSize( page->size() );
    for( int i = 0; i < 4; ++i )
        if( kViews[i] == v ) viewButton_[i]->setChecked( true );
}

void SSamplerEditor::refresh()
{
    if( closed_ ) return;
    bindProgram_();
    showView_( view() );
    keyView_->refreshFromModel();
    padView_->refreshFromModel();
    mapView_->refreshFromModel();
    zoneView_->refreshFromModel();
    QString name = track_ ? track_->getSName() : QString();
    const QString inst = slot_ ? QString::fromStdString( slot_->getDescriptor().name ) : QString();
    setWindowTitle( name.isEmpty() ? inst : QStringLiteral( "%1 - %2" ).arg( name, inst ) );
    syncTap();
}

QVector<std::uint32_t> SSamplerEditor::selectedZones() const
{
    SSamplerProgram *p = program();
    return p ? p->selectedZones() : QVector<std::uint32_t>();
}

QString SSamplerEditor::statusText() const { return status_ ? status_->text() : QString(); }

void SSamplerEditor::setStatus( const QString &text )
{
    if( status_ ) status_->setText( text );
}

// ---------------------------------------------------------------- submitting

namespace {

// Build ONE verb from its element, with the instrument's address implied when
// the element leaves it out. Null + `why` on a refusal.
SAction *buildVerb( QDomElement elem, const QString &trackPath, int slotIndex, QString *why )
{
    auto fail = [why]( const QString &w ) -> SAction * {
        if( why ) *why = w;
        return nullptr;
    };
    const QString verb = elem.tagName();
    if( !SSamplerXmlPane::verbs().contains( verb ) )
        return fail( QCoreApplication::translate( "SSamplerEditor",
                                                  "<%1> is not a verb of this instrument" )
                         .arg( verb ) );
    std::unique_ptr<SAction> act( SActionRegistry::instance().create( verb ) );
    if( !act ) return fail( QStringLiteral( "<%1> is not registered" ).arg( verb ) );
    const QStringList known = act->knownAttributes();
    const QDomNamedNodeMap attrs = elem.attributes();
    for( int i = 0; i < attrs.count(); ++i ) {
        const QString a = attrs.item( i ).nodeName();
        if( !known.isEmpty() && !known.contains( a ) )
            return fail( QCoreApplication::translate( "SSamplerEditor",
                                                      "<%1> has no attribute \"%2\"" )
                             .arg( verb, a ) );
    }
    if( trackPath.isEmpty() || slotIndex < 0 )
        return fail( QCoreApplication::translate( "SSamplerEditor",
                                                  "the instrument is no longer in the project" ) );
    // DESIGN §9.3: inside the instrument's own editor the pair is implied.
    if( !elem.hasAttribute( QStringLiteral( "trackPath" ) ) )
        elem.setAttribute( QStringLiteral( "trackPath" ), trackPath );
    if( !elem.hasAttribute( QStringLiteral( "slotIndex" ) ) )
        elem.setAttribute( QStringLiteral( "slotIndex" ), QString::number( slotIndex ) );
    if( !act->readXml( elem, 1 ) )
        return fail( QCoreApplication::translate( "SSamplerEditor", "<%1> could not be read" )
                         .arg( verb ) );
    return act.release();
}

// Submit `act` (owned by the history from here on); true when it was applied.
bool submitCounted( SAction *act, const QString &verb, QString *why )
{
    SApplication &app = SApplication::app();
    SActionHistory *h = app.actionHistory();
    const int before = h ? h->rejectedCount() : 0;
    app.submitAction( act );
    const int after = h ? h->rejectedCount() : 0;
    if( after != before ) {
        if( why )
            *why = QCoreApplication::translate( "SSamplerEditor",
                                                "<%1> was refused (the log says why)" )
                       .arg( verb );
        return false;
    }
    return true;
}

}  // namespace

bool SSamplerEditor::submitVerbXml( const QString &xml, quint64 gesture, QString *why )
{
    QDomDocument doc;
    // Both spellings, as action_roundtrip_test.cpp: Qt 6.4 (Ubuntu 24.04) has
    // no ParseResult and 6.5+ deprecates the out-parameters.
#if QT_VERSION >= QT_VERSION_CHECK( 6, 5, 0 )
    const QDomDocument::ParseResult parsed = doc.setContent( xml.trimmed() );
    const bool    parseOk   = bool( parsed );
    const QString parseErr  = parsed.errorMessage;
    const qint64  parseLine = parsed.errorLine;
    const qint64  parseCol  = parsed.errorColumn;
#else
    QString    parseErr;
    int        parseLine = 0, parseCol = 0;
    const bool parseOk = doc.setContent( xml.trimmed(), &parseErr, &parseLine, &parseCol );
#endif
    if( !parseOk ) {
        if( why )
            *why = tr( "Not one XML element: %1 (line %2, column %3)" )
                       .arg( parseErr )
                       .arg( parseLine )
                       .arg( parseCol );
        return false;
    }
    QDomElement elem = doc.documentElement();
    SAction *act = buildVerb( elem, currentTrackPath(), currentSlotIndex(), why );
    if( !act ) return false;
    act->setGesture( gesture );
    return submitCounted( act, elem.tagName(), why );
}

// THE DROP RULE (QBX-279; the simplest reading of DESIGN §9.3's "drag and drop
// onto a key, a pad or a zone", built to mirror §7's pad landing):
//   * a zone (the Zone view's, a Map rectangle): its layer 1 takes the source
//     (set-layer src); a zone with no layer gets one (add-layer);
//   * a key, or a pad's key: the ONE zone covering it is the target, as above;
//     no zone: a new one-key zone rooted there with the source as its layer;
//     more than one zone: REFUSED with a reason (drop on a zone instead).
// `file:<path>` is the layer's src as written; `asset:<name>` links the asset
// (proposal 56 §4.7), which only set-layer can do, so a new zone or layer for
// an asset is ONE composite: the add with an explicit id from nextId (QBX-265)
// and the set-layer naming the layer id that follows it. `media:` (a remote
// item) is refused: the media cache's fetch-then-place hook is not reachable
// from this module.
bool SSamplerEditor::dropOn( const QString &payload, int key, std::uint32_t zoneId, QString *why )
{
    auto fail = [why]( const QString &w ) {
        if( why ) *why = w;
        return false;
    };
    QString src;
    bool asset = false;
    if( payload.startsWith( QLatin1String( "file:" ) ) && payload.size() > 5 ) {
        src = payload;
    } else if( payload.startsWith( QLatin1String( "asset:" ) ) && payload.size() > 6 ) {
        src   = payload;
        asset = true;
    } else if( payload.startsWith( QLatin1String( "media:" ) ) ) {
        return fail( tr( "A remote media item cannot be dropped here yet: drop it on a track "
                         "first, or download it." ) );
    } else {
        return fail( tr( "Only files and assets can be dropped here." ) );
    }

    const audio::twSamplerProgramModel emptyModel;
    SSamplerProgram *prog = program();
    const audio::twSamplerProgramModel &m = prog ? prog->model() : emptyModel;

    if( !zoneId ) {
        if( key < 0 || key > 127 ) return fail( tr( "Nothing to drop on there." ) );
        const std::vector<std::uint32_t> covering = m.zonesCoveringKey( key );
        if( covering.size() > 1 ) {
            QStringList ids;
            for( std::uint32_t id : covering ) ids << QString::number( id );
            return fail( tr( "Key %1 is covered by zones %2: drop on one zone instead (Zone or "
                             "Map view)." )
                             .arg( ssamplerzones::keyName( key ), ids.join( QStringLiteral( ", " ) ) ) );
        }
        if( covering.size() == 1 ) zoneId = covering.front();
    }

    const QString trackPath = currentTrackPath();
    const int     slotIndex = currentSlotIndex();
    QDomDocument doc;
    QList<SAction *> verbs;
    auto add = [&]( const QDomElement &e ) -> bool {
        SAction *a = buildVerb( e, trackPath, slotIndex, why );
        if( !a ) return false;
        verbs.append( a );
        return true;
    };
    auto cleanup = [&]() { qDeleteAll( verbs ); verbs.clear(); };

    const QString k = QString::number( key );
    const QString next = QString::number( m.nextId() );
    if( !zoneId ) {
        // A new one-key zone rooted on the key.
        QDomElement z = verbElement( doc, QStringLiteral( "add-zone" ),
                                     { { QStringLiteral( "keyLo" ), k },
                                       { QStringLiteral( "keyHi" ), k },
                                       { QStringLiteral( "rootKey" ), k } } );
        if( asset ) z.setAttribute( QStringLiteral( "id" ), next );
        QDomElement layer = doc.createElement( QStringLiteral( "layer" ) );
        if( !asset ) layer.setAttribute( QStringLiteral( "src" ), src );
        z.appendChild( layer );
        if( !add( z ) ) { cleanup(); return false; }
        if( asset &&
            !add( verbElement( doc, QStringLiteral( "set-layer" ),
                               { { QStringLiteral( "layer" ), QString::number( m.nextId() + 1 ) },
                                 { QStringLiteral( "src" ), src } } ) ) ) {
            cleanup();
            return false;
        }
    } else {
        const std::vector<std::uint32_t> layers = m.layerIds( zoneId );
        if( !layers.empty() ) {
            if( !add( verbElement( doc, QStringLiteral( "set-layer" ),
                                   { { QStringLiteral( "layer" ), QString::number( layers.front() ) },
                                     { QStringLiteral( "src" ), src } } ) ) ) {
                cleanup();
                return false;
            }
        } else {
            QDomElement l = verbElement( doc, QStringLiteral( "add-layer" ),
                                         { { QStringLiteral( "zone" ), QString::number( zoneId ) } } );
            if( !asset ) l.setAttribute( QStringLiteral( "src" ), src );
            if( !add( l ) ) { cleanup(); return false; }
            if( asset && !add( verbElement( doc, QStringLiteral( "set-layer" ),
                                            { { QStringLiteral( "layer" ), next },
                                              { QStringLiteral( "src" ), src } } ) ) ) {
                cleanup();
                return false;
            }
        }
    }
    if( verbs.size() == 1 ) return submitCounted( verbs.front(), verbs.front()->name(), why );
    auto *comp = new SCompositeAction( verbs );
    return submitCounted( comp, QStringLiteral( "drop" ), why );
}

// ---------------------------------------------------------------- the tap

void SSamplerEditor::releaseTap_()
{
    if( fanout_ && sink_ ) fanout_->release( sink_ );
    fanout_ = nullptr;
    sink_   = nullptr;
    tapPort_.clear();
    tapChannel_ = -1;
    held_.clear();
    chord_.clear();
    if( tapTimer_ ) tapTimer_->stop();
}

void SSamplerEditor::syncTap()
{
    // Wanted: the track's MIDI input port, while the window is open (the
    // window exists only while it is).
    QString port;
    int channel = -1;
    if( closed_ ) return;
    if( track_ && track_->hasMidiTrackInput() ) {
        port    = track_->trackInputMidiPort();
        channel = track_->trackInputMidiChannel();
    }
    if( sink_ && port == tapPort_ && channel == tapChannel_ ) return;
    releaseTap_();
    if( port.isEmpty() ) return;
    SMidiInputHub *hub = SApplication::app().midiInputHub();
    audio::MidiInFanout *fan = hub ? hub->fanoutFor( port ) : nullptr;
    if( !fan ) return;
    const std::uint16_t mask = channel < 0 ? (std::uint16_t) 0xFFFF
                                           : (std::uint16_t)( 1u << channel );
    sink_ = fan->acquire( mask, "NassauDigital editor tap" );
    if( !sink_ ) {
        TW_LOGW( "samplerui", "[SAMPLER] no free MIDI input sink on port '%s' for the editor's "
                              "hand selection",
                 port.toStdString().c_str() );
        return;
    }
    fanout_     = fan;
    tapPort_    = port;
    tapChannel_ = channel;
    tapTimer_->start();
}

int SSamplerEditor::pollTap()
{
    if( !sink_ ) return 0;
    int n = 0;
    bool changed = false;
    audio::MidiInMessage msg;
    while( sink_->pop( msg ) ) {
        ++n;
        const int st = msg.status();
        if( msg.size < 3 ) continue;
        const int key = msg.bytes[1] & 0x7F;
        const int vel = msg.bytes[2] & 0x7F;
        if( st == 0x90 && vel > 0 ) {
            // A key pressed with none held starts a new chord: the selection
            // the last one left goes (the author: it stays until another key
            // is held).
            if( held_.isEmpty() ) chord_.clear();
            held_.insert( key );
            if( !chord_.contains( key ) ) chord_.append( key );
            changed = true;
        } else if( st == 0x80 || ( st == 0x90 && vel == 0 ) ) {
            if( held_.remove( key ) ) changed = true;
        }
    }
    if( changed ) applyHandSelection_();
    return n;
}

void SSamplerEditor::applyHandSelection_()
{
    keyView_->update();
    padView_->update();
    if( chord_.isEmpty() ) return;
    SSamplerProgram *p = program();
    if( !p ) return;
    // Every zone covering a key of the chord, in the order the keys came:
    // set DIRECTLY, not as select-zones (the author, 2026-11-03: hand
    // selection is not an undo step, like a device note's pad selection).
    QVector<std::uint32_t> ids;
    for( int key : chord_ )
        for( std::uint32_t id : p->model().zonesCoveringKey( key ) )
            if( !ids.contains( id ) ) ids.append( id );
    if( ids != p->rawSelection() ) p->setSelection( ids );
}

// ---------------------------------------------------------------- the seams

QString SSamplerEditor::describe() const
{
    SSamplerProgram *p = program();
    const View v = view();
    QWidget *cur = stack_->currentWidget();
    View page = View::Key;
    for( View c : kViews )
        if( pageFor_( c ) == cur ) page = c;

    QStringList sizes;
    bool fixedAll = true;
    for( View c : kViews ) {
        QWidget *w = pageFor_( c );
        sizes << QStringLiteral( "%1:%2" ).arg( viewName( c ), sizeText( w->minimumSize() ) );
        fixedAll = fixedAll && isFixed( w );
    }

    const audio::twSamplerProgramModel emptyModel;
    const audio::twSamplerProgramModel &m = p ? p->model() : emptyModel;
    const int bank = p ? p->padBank() : 0;
    QStringList padKeys;
    for( int pad = 0; pad < SSamplerProgram::kPadsPerBank; ++pad ) {
        const int k = m.padKey( SSamplerProgram::padNumberOf( bank, pad ) );
        padKeys << ( k < 0 ? QStringLiteral( "-" ) : QString::number( k ) );
    }
    QList<int> held = held_.values();
    std::sort( held.begin(), held.end() );
    QStringList heldText;
    for( int k : held ) heldText << QString::number( k );

    QStringList zones, gaps;
    for( const ssamplerzones::ZoneInfo &z : ssamplerzones::zones( m ) ) {
        zones << QStringLiteral( "%1:%2-%3/%4-%5" )
                     .arg( z.id ).arg( z.keyLo ).arg( z.keyHi ).arg( z.velLo ).arg( z.velHi );
        if( !z.gaps.isEmpty() ) {
            QStringList g;
            for( const auto &r : z.gaps ) g << QStringLiteral( "%1-%2" ).arg( r.first ).arg( r.second );
            gaps << QStringLiteral( "%1:%2" ).arg( z.id ).arg( g.join( QLatin1Char( ',' ) ) );
        }
    }

    SActionHistory *h = SApplication::app().actionHistory();
    const int undoIndex = ( h && h->undoStack() ) ? h->undoStack()->index() : -1;

    return QStringLiteral( "open=1" )
        + QStringLiteral( "|view=" ) + viewName( v )
        + QStringLiteral( "|page=" ) + viewName( page )
        + QStringLiteral( "|pageSize=" ) + sizeText( stack_->minimumSize() )
        + QStringLiteral( "|pageFixed=%1" ).arg( isFixed( stack_ ) && isFixed( cur ) &&
                                                 stack_->minimumSize() == cur->minimumSize()
                                                     ? 1 : 0 )
        + QStringLiteral( "|sizes=" ) + sizes.join( QLatin1Char( ',' ) )
        + QStringLiteral( "|fixedAll=%1" ).arg( fixedAll ? 1 : 0 )
        + QStringLiteral( "|program=%1" ).arg( p ? 1 : 0 )
        + QStringLiteral( "|bank=%1" ).arg( bank )
        + QStringLiteral( "|pad=%1" ).arg( p ? p->pad() : 0 )
        + QStringLiteral( "|padKeys=" ) + padKeys.join( QLatin1Char( ',' ) )
        + QStringLiteral( "|selected=" ) + idList( selectedZones() )
        + QStringLiteral( "|held=" ) + heldText.join( QLatin1Char( ';' ) )
        + QStringLiteral( "|tap=%1" ).arg( sink_ ? 1 : 0 )
        + QStringLiteral( "|zones=" ) + zones.join( QLatin1Char( ';' ) )
        + QStringLiteral( "|gaps=" ) + gaps.join( QLatin1Char( ';' ) )
        + QStringLiteral( "|zoneShown=" )
        + ( zoneView_->shownZone() ? QString::number( zoneView_->shownZone() ) : QStringLiteral( "-" ) )
        + QStringLiteral( "|knobs=" ) + zoneView_->describeKnobs()
        + QStringLiteral( "|completions=" ) + xmlPane_->offered().join( QLatin1Char( ',' ) )
        + QStringLiteral( "|xml=" ) + xmlPane_->text()
        + QStringLiteral( "|xmlStatus=" ) + xmlPane_->status()
        + QStringLiteral( "|undoIndex=%1" ).arg( undoIndex )
        + QStringLiteral( "|title=" ) + windowTitle()
        + QStringLiteral( "|status=" ) + statusText();
}

bool SSamplerEditor::clickView( const QString &v )
{
    for( int i = 0; i < 4; ++i )
        if( viewName( kViews[i] ) == v ) {
            viewButton_[i]->click();
            return true;
        }
    return false;
}

bool SSamplerEditor::clickBank( int bank ) { return padView_->clickBank( bank ); }

namespace {

void sendPress( QWidget *w, const QPoint &pos )
{
    QMouseEvent press( QEvent::MouseButtonPress, QPointF( pos ), w->mapToGlobal( QPointF( pos ) ),
                       Qt::LeftButton, Qt::LeftButton, Qt::NoModifier );
    QCoreApplication::sendEvent( w, &press );
    QMouseEvent release( QEvent::MouseButtonRelease, QPointF( pos ),
                         w->mapToGlobal( QPointF( pos ) ), Qt::LeftButton, Qt::NoButton,
                         Qt::NoModifier );
    QCoreApplication::sendEvent( w, &release );
}

}  // namespace

bool SSamplerEditor::clickPad( int pad )
{
    if( pad < 0 || pad >= SSamplerProgram::kPadsPerBank ) return false;
    if( stack_->currentWidget() != padView_ ) return false;   // a user clicks what is shown
    sendPress( padView_, SSamplerPadView::cellRect( pad ).center() );
    return true;
}

bool SSamplerEditor::clickKey( int key )
{
    if( key < 0 || key > 127 || stack_->currentWidget() != keyView_ ) return false;
    sendPress( keyView_, SSamplerKeyView::keyCentre( key ) );
    return true;
}

bool SSamplerEditor::clickMap( int key, int vel )
{
    if( key < 0 || key > 127 || vel < 0 || vel > 127 || stack_->currentWidget() != mapView_ )
        return false;
    sendPress( mapView_, SSamplerMapView::pointFor( key, vel ) );
    return true;
}

QWidget *SSamplerEditor::knobById_( const QString &id ) const
{
    return zoneView_->dialFor( id );
}

bool SSamplerEditor::knobPress( const QString &id )
{
    auto *d = qobject_cast<QAbstractSlider *>( knobById_( id ) );
    if( !d || !d->isEnabled() || stack_->currentWidget() != zoneView_ ) return false;
    d->setSliderDown( true );   // Qt's own spelling of what a press does
    return true;
}

bool SSamplerEditor::knobSet( const QString &id, double value )
{
    if( stack_->currentWidget() != zoneView_ ) return false;
    return zoneView_->setKnob( id, value );
}

bool SSamplerEditor::knobRelease( const QString &id )
{
    auto *d = qobject_cast<QAbstractSlider *>( knobById_( id ) );
    if( !d ) return false;
    d->setSliderDown( false );
    return true;
}

bool SSamplerEditor::xmlType( const QString &text, int cursor )
{
    xmlPane_->typeText( text, cursor );
    return true;
}

bool SSamplerEditor::xmlAcceptCompletion( const QString &choice )
{
    return xmlPane_->acceptCompletion( choice );
}

bool SSamplerEditor::xmlApply()
{
    // Through the button, so the clicked -> apply wiring is on the path.
    auto *b = findChild<QPushButton *>( QStringLiteral( "xmlApply" ) );
    if( !b ) return false;
    b->click();
    return xmlPane_->status() == tr( "Applied." );
}

bool SSamplerEditor::dropResource( const QString &payload, const QString &target )
{
    SSamplerViewBase *view = nullptr;
    QPoint pos;
    const QStringList t = target.split( QLatin1Char( ':' ) );
    if( t.value( 0 ) == QLatin1String( "key" ) && t.size() == 2 ) {
        view = keyView_;
        pos  = SSamplerKeyView::keyCentre( t[1].toInt() );
    } else if( t.value( 0 ) == QLatin1String( "pad" ) && t.size() == 2 ) {
        view = padView_;
        pos  = SSamplerPadView::cellRect( t[1].toInt() ).center();
    } else if( t.value( 0 ) == QLatin1String( "map" ) && t.size() == 3 ) {
        view = mapView_;
        pos  = SSamplerMapView::pointFor( t[1].toInt(), t[2].toInt() );
    } else if( t.value( 0 ) == QLatin1String( "zone" ) ) {
        view = zoneView_;
        pos  = QPoint( 10, 10 );
    }
    if( !view ) return false;
    if( stack_->currentWidget() != view ) {
        setStatus( tr( "Drop target is not the view shown." ) );
        return false;
    }
    QMimeData mime;
    mime.setData( QStringLiteral( "application/x-smaragd-resource" ), payload.toUtf8() );
    QDragEnterEvent enter( pos, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier );
    QCoreApplication::sendEvent( view, &enter );
    if( !enter.isAccepted() ) return false;
    QDropEvent drop( QPointF( pos ), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier );
    QCoreApplication::sendEvent( view, &drop );
    return view->lastDropSubmitted();
}
