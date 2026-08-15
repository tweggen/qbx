#include "app/objects/midi/smidisequence.h"

#include <QDebug>
#include <QDomElement>
#include <QStringList>
#include <QTextStream>
#include <algorithm>
#include <cstring>

#include "app/model/sappcontext.h"
#include "app/model/slink.h"
#include "app/model/sproject.h"
#include "app/objects/midi/smidirndrinline.h"
#include "app/persistence/sprojectloader.h"
#include "tw/graph/twcomponent.h"
#include "tw/events/twtempomap.h"

// ---------------------------------------------------------------------------
// A private SILENCE component.
//
// Exactly the STakeSilence trick (objects/cut): an event object has no audio,
// and a NULL root component makes twView::getComponent() warn once per freeze
// forever (fact M5). objects/midi may not include tw/mix, so this is a bare
// twComponent rather than a twRewire.
// ---------------------------------------------------------------------------
namespace {

class SMidiSilence : public twComponent
{
public:
    explicit SMidiSilence( tw303aEnvironment &e ) : twComponent( e ) {}
    bool isSeekable() const override { return true; }
    int seekTo( offset_t ) override { return 0; }
    void reset() override {}
    length_t renderFrames( sample_t *out, length_t n, const sample_t *,
                           length_t, idx_t ) override {
        ::memset( out, 0, n * sizeof( sample_t ) );
        return n;
    }
    void createOutputLatches() override {}
    idx_t getNInputs() const override { return 0; }
    idx_t getNOutputs() const override { return 1; }
    const char *getInputName( idx_t ) const override { return nullptr; }
    const char *getOutputName( idx_t ) const override { return "silence"; }
};

// The kind ordering inside one tick. Note-offs settle before controllers and
// controllers before note-ons - the same rule twEventBlock::sortEvents uses,
// so the model's stored order and the engine's emitted order agree.
int kindRank( twEventKind k )
{
    switch( k ) {
    case twEventKind::NoteOff:
    case twEventKind::NoteChoke:
    case twEventKind::NoteEnd:   return 0;
    case twEventKind::NoteOn:    return 2;
    default:                     return 1;
    }
}

struct KindName { twEventKind kind; const char *name; };

// One spelling per kind. Short, lower case, stable: these strings are in every
// project file from now on, so they are appended to, never renamed.
const KindName kKindNames[] = {
    { twEventKind::NoteOn,            "note" },
    { twEventKind::NoteOff,           "noteoff" },
    { twEventKind::NoteChoke,         "notechoke" },
    { twEventKind::NoteEnd,           "noteend" },
    { twEventKind::NoteExpression,    "noteexpr" },
    { twEventKind::PolyPressure,      "polypressure" },
    { twEventKind::ControlChange,     "cc" },
    { twEventKind::PitchBend,         "bend" },
    { twEventKind::ChannelPressure,   "pressure" },
    { twEventKind::ProgramChange,     "program" },
    { twEventKind::Sysex,             "sysex" },
    { twEventKind::Midi1,             "midi1" },
    { twEventKind::ParamValue,        "param" },
    { twEventKind::ParamMod,          "parammod" },
    { twEventKind::ParamGestureBegin, "gesturebegin" },
    { twEventKind::ParamGestureEnd,   "gestureend" },
    { twEventKind::Transport,         "transport" },
    { twEventKind::Tempo,             "tempo" },
    { twEventKind::TimeSig,           "timesig" },
    { twEventKind::KeySig,            "keysig" },
    { twEventKind::Marker,            "marker" },
    { twEventKind::Lyric,             "lyric" },
    { twEventKind::ChordSymbol,       "chord" },
    { twEventKind::Articulation,      "articulation" },
    { twEventKind::StringFret,        "stringfret" },
    { twEventKind::TrackerCell,       "trackercell" },
    { twEventKind::Text,              "text" },
    { twEventKind::NoteAttr,          "noteattr" },
};

}  // namespace

// ---------------------------------------------------------------------------
// SEvent
// ---------------------------------------------------------------------------

bool SEvent::operator<( const SEvent &o ) const
{
    if( t != o.t ) return t < o.t;
    const int ra = kindRank( kind ), rb = kindRank( o.kind );
    if( ra != rb ) return ra < rb;
    if( kind != o.kind ) return (int) kind < (int) o.kind;
    if( channel != o.channel ) return channel < o.channel;
    if( key != o.key ) return key < o.key;
    if( paramId != o.paramId ) return paramId < o.paramId;
    if( value != o.value ) return value < o.value;
    return dur < o.dur;
}

// ---------------------------------------------------------------------------
// smidievents
// ---------------------------------------------------------------------------

QString smidievents::kindToString( const SEvent &e )
{
    if( !e.rawKind.isEmpty() ) return e.rawKind;
    for( const KindName &kn : kKindNames )
        if( kn.kind == e.kind ) return QString::fromLatin1( kn.name );
    // The only remaining kind is Unknown: spell it as the SMF meta type it
    // carries, in hex, so it stays identifiable across builds.
    return QString( "0x%1" ).arg( e.paramId, 2, 16, QLatin1Char( '0' ) );
}

bool smidievents::kindFromString( const QString &s, twEventKind &kind,
                                  quint32 &paramId )
{
    for( const KindName &kn : kKindNames ) {
        if( s.compare( QLatin1String( kn.name ), Qt::CaseInsensitive ) == 0 ) {
            kind = kn.kind;
            return true;
        }
    }
    if( s.startsWith( "0x", Qt::CaseInsensitive ) ) {
        bool ok = false;
        const uint v = s.mid( 2 ).toUInt( &ok, 16 );
        if( ok ) { kind = twEventKind::Unknown; paramId = v; return true; }
    }
    return false;
}

SEvent smidievents::readEvent( const QDomElement &el )
{
    SEvent ev;
    const QString k = el.attribute( "k", "note" );
    quint32 metaType = 0;
    if( !kindFromString( k, ev.kind, metaType ) ) {
        // A kind this build does not know. Keep the ELEMENT verbatim: the
        // spelling, and every attribute on it, so a file written by a newer
        // build survives a load/save round trip here untouched.
        ev.kind = twEventKind::Unknown;
        ev.rawKind = k;
    } else if( ev.kind == twEventKind::Unknown ) {
        ev.paramId = metaType;
    }

    ev.t   = el.attribute( "t", "0" ).toLongLong();
    ev.dur = el.attribute( "d", "0" ).toLongLong();
    if( el.hasAttribute( "ch" ) )  ev.channel = el.attribute( "ch" ).toInt();
    if( el.hasAttribute( "key" ) ) ev.key = el.attribute( "key" ).toInt();
    if( el.hasAttribute( "p" ) )   ev.paramId = el.attribute( "p" ).toUInt();
    if( el.hasAttribute( "v" ) )   ev.value = el.attribute( "v" ).toDouble();
    if( el.hasAttribute( "v2" ) )  ev.value2 = el.attribute( "v2" ).toDouble();
    if( el.hasAttribute( "text" ) ) ev.text = el.attribute( "text" );
    if( el.hasAttribute( "blob" ) )
        ev.blob = QByteArray::fromHex( el.attribute( "blob" ).toLatin1() );
    ev.muted = el.attribute( "m", "0" ).toInt() != 0;

    // Anything we did not consume rides along verbatim.
    static const char *known[] = { "k", "t", "d", "ch", "key", "p", "v",
                                   "v2", "text", "blob", "m" };
    const QDomNamedNodeMap attrs = el.attributes();
    for( int i = 0; i < attrs.count(); ++i ) {
        const QDomNode n = attrs.item( i );
        const QString name = n.nodeName();
        bool isKnown = false;
        for( const char *kn : known )
            if( name == QLatin1String( kn ) ) { isKnown = true; break; }
        if( !isKnown ) ev.extra.insert( name, n.nodeValue() );
    }
    return ev;
}

namespace {
QString xmlAttr( const QString &s )
{
    return s.toHtmlEscaped().replace( QLatin1Char( '\'' ), QLatin1String( "&apos;" ) );
}
}  // namespace

void smidievents::writeEvent( QTextStream &o, const SEvent &e )
{
    o << "  <e k='" << xmlAttr( kindToString( e ) ) << "'"
      << " t='" << e.t << "'";
    if( e.dur != 0 )      o << " d='" << e.dur << "'";
    if( e.channel >= 0 )  o << " ch='" << e.channel << "'";
    if( e.key >= 0 )      o << " key='" << e.key << "'";
    if( e.paramId != 0 )  o << " p='" << e.paramId << "'";
    if( e.value != 0.0 )  o << " v='" << e.value << "'";
    if( e.value2 != 0.0 ) o << " v2='" << e.value2 << "'";
    if( !e.text.isEmpty() ) o << " text='" << xmlAttr( e.text ) << "'";
    if( !e.blob.isEmpty() )
        o << " blob='" << QString::fromLatin1( e.blob.toHex() ) << "'";
    if( e.muted )         o << " m='1'";
    for( auto it = e.extra.constBegin(); it != e.extra.constEnd(); ++it )
        o << " " << it.key() << "='" << xmlAttr( it.value() ) << "'";
    o << "/>\n";
}

std::shared_ptr<const twEventSeq> smidievents::buildSeq(
    const std::vector<SEvent> &events, const Fraction &scale,
    int transpose, double velocityScale, int channelOverride )
{
    twEventSeqBuilder b;
    b.reserve( events.size() );
    for( const SEvent &e : events ) {
        if( e.muted ) continue;
        twEvent te;
        te.time = ( Fraction( e.t ) * scale ).floorToInt();
        te.kind = e.kind;
        te.channel = ( channelOverride >= 0 && e.channel >= 0 )
                       ? (int16_t) channelOverride : (int16_t) e.channel;
        te.key = (int16_t) e.key;
        te.paramId = e.paramId;
        te.value = e.value;
        te.value2 = e.value2;
        if( e.kind == twEventKind::NoteOn ) {
            // Transposition and velocity scaling are per-CLIP modifiers, so
            // they are applied HERE, when the clip re-expresses the shared
            // content - never written back into the sequence.
            if( te.key >= 0 ) {
                int k = te.key + transpose;
                if( k < 0 ) k = 0;
                if( k > 127 ) k = 127;
                te.key = (int16_t) k;
            }
            double v = e.value * velocityScale;
            if( v < 0.0 ) v = 0.0;
            if( v > 127.0 ) v = 127.0;
            te.value = v;
            // A duration must be at least one frame after scaling, or a note
            // would be born already closed.
            int64_t d = ( Fraction( e.dur ) * scale ).floorToInt();
            if( e.dur > 0 && d < 1 ) d = 1;
            te.duration = d;
        }
        if( !e.blob.isEmpty() )
            b.addWithPayload( te, e.blob.constData(), (size_t) e.blob.size() );
        else if( !e.text.isEmpty() ) {
            const QByteArray utf8 = e.text.toUtf8();
            b.addWithPayload( te, utf8.constData(), (size_t) utf8.size() );
        } else
            b.add( te );
    }
    return b.build();
}

// ---------------------------------------------------------------------------
// SMidiSequence
// ---------------------------------------------------------------------------

SMidiSequence::SMidiSequence( SProject *project )
    : SObject( project )
{
    std::lock_guard<std::mutex> lock( mutex() );
    rebuild_nolock();
}

SMidiSequence::~SMidiSequence()
{
}

std::vector<SEvent> SMidiSequence::events() const
{
    std::lock_guard<std::mutex> lock( mutex() );
    return events_;
}

int SMidiSequence::eventCount() const
{
    std::lock_guard<std::mutex> lock( mutex() );
    return (int) events_.size();
}

void SMidiSequence::setEvents( std::vector<SEvent> events )
{
    {
        std::lock_guard<std::mutex> lock( mutex() );
        events_ = std::move( events );
        rebuild_nolock();
    }
    // The consumer is class-1 (a synth voice carries state across pages), so an
    // event edit is never bounded on the right: everything from 0 on is dirty
    // (design 3.2, F9). The windows over this content relay it upward.
    emit eventsChanged( 0 );
    emit durationChanged( getDuration() );
}

qint64 SMidiSequence::lengthTicks() const
{
    std::lock_guard<std::mutex> lock( mutex() );
    return lengthTicks_ > 0 ? lengthTicks_ : contentEndTicks_;
}

void SMidiSequence::setLengthTicks( qint64 ticks )
{
    {
        std::lock_guard<std::mutex> lock( mutex() );
        lengthTicks_ = ticks > 0 ? ticks : 0;
    }
    emit eventsChanged( 0 );
    emit durationChanged( getDuration() );
}

std::shared_ptr<const twEventSeq> SMidiSequence::tickSnapshot() const
{
    std::lock_guard<std::mutex> lock( mutex() );
    return tickSeq_;
}

void SMidiSequence::rebuild_nolock()
{
    std::sort( events_.begin(), events_.end() );
    contentEndTicks_ = 0;
    for( const SEvent &e : events_ )
        contentEndTicks_ = std::max( contentEndTicks_, e.endTick() );
    // Identity scale: the table stays in the domain the model stores, TICKS.
    tickSeq_ = smidievents::buildSeq( events_, Fraction( 1 ) );
}

length_t SMidiSequence::getDuration() const
{
    SProject *project = getProjectSafe();
    if( !project ) return 0;
    const qint64 ticks = lengthTicks();
    if( ticks <= 0 ) return 0;
    // The ONE converter (events/CONTRACT inv. 4); rounding is explicit here,
    // at the render boundary, exactly as for the warp map.
    return (length_t) project->tempoMap()
        .ticksToFrames( TickLen( (int64_t) ticks ), project->getSRate() )
        .floorToInt();
}

std::shared_ptr<twComponent> SMidiSequence::getRootComponent()
{
    std::lock_guard<std::mutex> lock( mutex() );
    if( !cpSilence_ ) {
        tw303aEnvironment *env = SAppContext::get().get303aEnvironment();
        if( !env ) return nullptr;
        cpSilence_ = std::make_shared<SMidiSilence>( *env );
        cpSilence_->init();
    }
    return cpSilence_;
}

QWidget *SMidiSequence::getDetailEditWidget( QWidget * ) { return nullptr; }
QWidget *SMidiSequence::getInlineEditWidget( QWidget * ) { return nullptr; }

SObjectRenderer *SMidiSequence::getInlineRenderer()
{
    if( !inlineRenderer_ ) inlineRenderer_ = new SMidiSequenceRendererInline( *this );
    return (SObjectRenderer *) inlineRenderer_;
}

int SMidiSequence::serializeSelfAttributes( QTextStream &o )
{
    o << " ppq='" << ppq_ << "'";
    {
        std::lock_guard<std::mutex> lock( mutex() );
        if( lengthTicks_ > 0 ) o << " lengthTicks='" << lengthTicks_ << "'";
    }
    const char *originName = origin_ == Origin::Smf ? "smf"
                           : origin_ == Origin::Recorded ? "recorded" : "drawn";
    o << " origin='" << originName << "'";
    SObject::serializeSelfAttributes( o );
    return 0;
}

int SMidiSequence::serialize( QTextStream &o )
{
    o << "<SMidiSequence";
    if( serializeSelfAttributes( o ) < 0 ) return -1;
    o << ">\n";
    // INLINE, sorted on write (proposal 32 diff stability). The loader ignores
    // <events> for its <SLink>-based ordering - it is a payload, not a
    // reference (model/CONTRACT: the sanctioned non-SLink payload).
    std::vector<SEvent> snapshot;
    {
        std::lock_guard<std::mutex> lock( mutex() );
        snapshot = events_;
    }
    o << " <events count='" << (int) snapshot.size() << "'>\n";
    for( const SEvent &e : snapshot ) smidievents::writeEvent( o, e );
    o << " </events>\n";
    o << "</SMidiSequence>\n";
    return 0;
}

int SMidiSequence::readPreChildrenAttributes( QDomElement &element )
{
    SObject::readPreChildrenAttributes( element );

    bool ok = false;
    const int p = element.attribute( "ppq", QString::number( DEFAULT_PPQ ) )
                      .toInt( &ok );
    ppq_ = ( ok && p > 0 ) ? p : DEFAULT_PPQ;

    const QString origin = element.attribute( "origin", "drawn" );
    origin_ = origin == "smf" ? Origin::Smf
            : origin == "recorded" ? Origin::Recorded : Origin::Drawn;

    std::vector<SEvent> loaded;
    QDomNode n = element.firstChild();
    while( !n.isNull() ) {
        if( n.isElement() && n.nodeName() == "events" ) {
            QDomNode ev = n.firstChild();
            while( !ev.isNull() ) {
                if( ev.isElement() && ev.nodeName() == "e" )
                    loaded.push_back( smidievents::readEvent( ev.toElement() ) );
                ev = ev.nextSibling();
            }
            break;
        }
        n = n.nextSibling();
    }

    {
        std::lock_guard<std::mutex> lock( mutex() );
        events_ = std::move( loaded );
        lengthTicks_ = element.attribute( "lengthTicks", "0" ).toLongLong();
        if( lengthTicks_ < 0 ) lengthTicks_ = 0;
        rebuild_nolock();
    }
    return 0;
}

SLink *SMidiSequence::instantiateFromDomElement(
    SProjectLoader &projectLoader, QDomElement &element, SObject *parent )
{
    SMidiSequence *seq = new SMidiSequence( &projectLoader.getProject() );
    seq->readPreChildrenAttributes( element );

    // Construct with parent=NULL, then setParent (slink.h rule).
    SLink *link = new SLink( *seq, NULL );
    if( parent ) link->setParent( parent );
    return link;
}

// Self-registration (proposal 14 Phase 5): PLAIN, because a sequence has no
// <SLink> content of its own - it IS the content. An unresolvable link
// somewhere else never drags it down, and it never drags anything down.
static const bool s_registered_smidisequence =
    ( SProjectLoader::registerSObjectClass( "SMidiSequence",
          SMidiSequence::instantiateFromDomElement,
          SElementKind::Plain ), true );
