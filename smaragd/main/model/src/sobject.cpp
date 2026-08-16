
#include <stdlib.h>
#include <math.h>
#include <cstdint>

#include <qobject.h>
#include <qtextstream.h>
#include <QChildEvent>
#include <QEvent>
#include <QThread>

#include "app/model/sobject.h"
#include "tw/core/twlog.h"
#include "tw/schedule/capture_aspects.h"  // Preview/Playback/Metadata/Export bits
#include "app/model/slink.h"
#include "app/model/sproject.h"
#include "tw/graph/twcomponent.h"
#include "tw/pages/tw_output_page.h"
#include "tw/sources/twrandomsource.h"
#include "tw/schedule/capture_revalidator.h"

void SObject::setSolo( bool f )
{
    if( f==solo_ ) return;
    solo_ = f;
    emit soloChanged( f );
    // Solo changes both preview rendering (affects visibility in composites)
    // and audio routing. Invalidate all three aspects.
    notifyDependentsChanged(Preview | Playback | Metadata);
}

void SObject::setMuted( bool f )
{
    if( f==muted_ ) return;
    muted_ = f;
    emit mutedChanged( f );
    // Mute changes both preview rendering (affects visibility in composites)
    // and audio routing. Invalidate all three aspects.
    notifyDependentsChanged(Preview | Playback | Metadata);
}

void SObject::setArmedForRecording( bool f )
{
    if( f==armed_ ) return;
    armed_ = f;
    emit armedForRecordingChanged( f );
    // Armed state doesn't affect audio capture aspects (only matters during record),
    // but invalidate for UI consistency
    notifyDependentsChanged(Metadata);
}

void SObject::setRecordingChannels( uint32_t channels )
{
    if( channels == recordingChannels_ ) return;
    recordingChannels_ = channels;
    emit recordingChannelsChanged( channels );
}

void SObject::setVolume( double d )
{
    {
        std::lock_guard<std::mutex> lock( volumeMutex_ );
        if( fabs( volume_-d ) < 0.0001 ) return;
        volume_ = d;
    }
    // Volume change affects preview rendering, so invalidate the cached preview
    // so it gets regenerated at the new volume level (not just scaled on-the-fly).
    invalidatePreview();
    emit volumeChanged( d );
    // Volume changes audio content but not arrangement:
    // notify dependents to invalidate Playback + Metadata aspects (audio content change)
    notifyDependentsChanged(Playback | Metadata);
}

void SObject::setPan( double d )
{
    if( d<-1.0 ) d = -1.0;
    else if( d>1.0 ) d= 1.0;
    if( fabs( pan_-d ) < 0.00001 ) return;
    pan_ = d;
    emit panChanged( d );
}

void SObject::setDelay( double d )
{
    if( fabs( delay_-d ) < 0.000001 ) return;
    delay_ = d;
    emit delayChanged( d );
}

void SObject::setEditGroup( int id )
{
    if( editGroup_ == id ) return;
    editGroup_ = id;
    emit editGroupChanged( id );
}

void SObject::setSName( const QString &n )
{
    // Stored VERBATIM, empty included. The old body was
    //     QString newName;
    //     if( n=="" ) newName = "(untitled)";
    // — the non-empty branch was simply missing, so every real name (generated
    // track names, asset names, plugin names) was silently stored as "", and
    // only the empty input produced a value. Two reasons the substitution is
    // gone rather than repaired: every reader already spells "unnamed" as
    // getSName().isEmpty() (e.g. SCutRendererInline), and set-clip-name's undo
    // must restore the previous name EXACTLY — mapping "" to "(untitled)" makes
    // clearing a name unundoable.
    if( n==sName_ ) return;
    sName_ = n;
    emit sNameChanged( n );
}

int SObject::serializeSelfAttributes( QTextStream &o )
{
    o << " id='" << reinterpret_cast<std::uintptr_t>(this) << "'"
      << " nRefs='" << nRefs_ << "'"
      << " hasDuration='" << hasDuration() << "'";
    if( hasDuration() ) {
        // Store duration as time (seconds) for rate independence
        int srate = parent() && parent()->parent() ?
            dynamic_cast<SProject*>(parent()->parent())->getSRate() : 48000;
        double durationSec = getDuration() / (double)srate;
        o << " durationSec='" << durationSec << "'";
    }
    o << " muted='";
    o << (isMuted()?"true":"false") << "'";
    o << " solo='";
    o << (isSolo()?"true":"false") << "'";
    o << " armedForRecording='";
    o << (isArmedForRecording()?"true":"false") << "'";
    o << " volume='" << getVolume() << "'";
    o << " pan='" << getPan() << "'";
    o << " delay='" << getDelay() << "'";
    if( editGroup_ != 0 )
        o << " editGroup='" << editGroup_ << "'";
    // Written only when the object carries a name the USER chose. Every
    // SObject is constructed with DEFAULT_SNAME, so serializing unconditionally
    // would stamp a meaningless sName on every object in every project file;
    // skipping the placeholder keeps untouched projects byte-unchanged. The
    // value is user-typed (proposal 31's clip name), so unlike the raw
    // `filename` attribute it must be escaped — including the apostrophe, which
    // delimits the attribute.
    if( !sName_.isEmpty() && sName_ != QLatin1String( DEFAULT_SNAME ) )
        o << " sName='"
          << sName_.toHtmlEscaped().replace( QLatin1Char('\''),
                                             QLatin1String("&apos;") )
          << "'";
    return 0;
}

int SObject::readPreChildrenAttributes( QDomElement &element )
{
    QString data;

    // Load duration: try new time-based format first (durationSec), then
    // fall back to old sample-based format for backwards compatibility
    int srate = parent() && parent()->parent() ?
        dynamic_cast<SProject*>(parent()->parent())->getSRate() : 48000;

    QString durationSecStr = element.attribute( "durationSec" );
    if( !durationSecStr.isEmpty() ) {
        // New format: stored as seconds, convert to samples
        double durationSec = durationSecStr.toDouble();
        setDuration( (length_t)( durationSec * srate + 0.5 ) );
    } else {
        // Backwards compatibility: old format was samples, default 10000 @ 48kHz
        data = element.attribute( "duration" );
        if( data.isEmpty() ) {
            // Default: ~208 ms (10000 samples @ 48kHz)
            setDuration( (length_t)( 0.208333 * srate + 0.5 ) );
        } else {
            length_t oldSamples = data.toULongLong();
            // Assume old value was at 48kHz, scale proportionally
            setDuration( (length_t)( ( oldSamples * srate ) / 48000.0 + 0.5 ) );
        }
    }
    data = element.attribute( "muted", "false" );
    setMuted( data.startsWith( "true" ) );
    data = element.attribute( "solo", "false" );
    setSolo( data.startsWith( "true" ) );
    data = element.attribute( "armedForRecording", "false" );
    setArmedForRecording( data.startsWith( "true" ) );
    data = element.attribute( "volume", "1.0" );
    setVolume( data.toDouble() );
    data = element.attribute( "pan", "0.0" );
    setPan( data.toDouble() );
    data = element.attribute( "delay", "0.0" );
    setDelay( data.toDouble() );
    setEditGroup( element.attribute( "editGroup", "0" ).toInt() );
    // Absent on projects saved before proposal 31, and absent for unnamed
    // objects. Only assign when there is something to assign: setSName("")
    // means "(untitled)", which would turn every unnamed object into a named
    // one on the next save.
    data = element.attribute( "sName" );
    if( !data.isEmpty() )
        setSName( data );
    return 0;
}

int SObject::readPostChildrenAttributes( QDomElement &element )
{
    readAutomation( element );
    return 0;
}

int SObject::serialize( QTextStream &o )
{
    int res;
    o << "<" << metaObject()->className();
    res = serializeSelfAttributes( o );
    if( res<0 ) return res;
    o  << ">\n";

    serializeAutomation( o );

    for( SLink *lk : childLinks() ) {
        int res = lk->serialize( o );
        if( res<0 ) break;
    }

    o << "</" << metaObject()->className() << ">\n";
    return 0;
}

// --- automation lanes (proposal 37 P5, design 3.3) ---------------------------
//
// Inline, and a NON-SLink child on purpose: the loader orders and resolves on
// <SLink> children only, so an older build reads this element, finds nothing it
// recognises, and ignores it. That tolerance is the whole reason lanes are not
// top-level objects (design 3.3).

SAutomationLane *SObject::automationLane( const QString &target ) const
{
    const QString norm = SParamRef::parse( target ).toString();
    const QString key  = norm.isEmpty() ? target : norm;
    for( const std::unique_ptr<SAutomationLane> &l : automationLanes_ )
        if( l && l->target() == key ) return l.get();
    return nullptr;
}

SAutomationLane *SObject::ensureAutomationLane( const QString &target )
{
    if( SAutomationLane *l = automationLane( target ) ) return l;
    if( !SParamRef::parse( target ).isValid() ) return nullptr;
    automationLanes_.push_back(
        std::unique_ptr<SAutomationLane>( new SAutomationLane( target ) ) );
    return automationLanes_.back().get();
}

bool SObject::removeAutomationLane( const QString &target )
{
    const QString norm = SParamRef::parse( target ).toString();
    const QString key  = norm.isEmpty() ? target : norm;
    for( auto it = automationLanes_.begin(); it != automationLanes_.end(); ++it ) {
        if( *it && ( *it )->target() == key ) {
            automationLanes_.erase( it );
            return true;
        }
    }
    return false;
}

QList<SAutomationLane *> SObject::automationLanes() const
{
    QList<SAutomationLane *> out;
    for( const std::unique_ptr<SAutomationLane> &l : automationLanes_ )
        if( l ) out.append( l.get() );
    return out;
}

std::shared_ptr<const twAutomationCurve>
SObject::automationCurve( const QString &target ) const
{
    SAutomationLane *l = automationLane( target );
    return l ? l->snapshot() : nullptr;
}

// The DEFAULT is the invalidation and nothing else. Correct for an owner with
// no engine component of its own to push into; the four real owners override.
void SObject::onAutomationChanged( SAutomationLane &lane, offset_t start, offset_t end )
{
    (void) lane;
    if( end <= start ) return;
    invalidateRenderPathRange( start, end );
}

void SObject::copyAutomationFrom( const SObject &src )
{
    automationLanes_.clear();
    for( const std::unique_ptr<SAutomationLane> &l : src.automationLanes_ ) {
        if( !l ) continue;
        std::unique_ptr<SAutomationLane> copy( new SAutomationLane( l->target() ) );
        copy->setParamName( l->paramName() );
        copy->setMode( l->mode() );
        copy->setPoints( l->points() );
        automationLanes_.push_back( std::move( copy ) );
    }
    applyAutomationToEngine();
}

int SObject::serializeAutomation( QTextStream &o )
{
    if( automationLanes_.empty() ) return 0;
    // A lane with no points is still WRITTEN: its mode is user state (an armed
    // Write lane that has recorded nothing yet must survive a save).
    o << "<automation>\n";
    for( const std::unique_ptr<SAutomationLane> &l : automationLanes_ )
        if( l ) l->serialize( o );
    o << "</automation>\n";
    return 0;
}

int SObject::readAutomation( const QDomElement &element )
{
    const QDomElement autoEl = element.firstChildElement( "automation" );
    if( autoEl.isNull() ) return 0;

    for( QDomNode n = autoEl.firstChild(); !n.isNull(); n = n.nextSibling() ) {
        if( !n.isElement() ) continue;
        const QDomElement laneEl = n.toElement();
        if( laneEl.tagName() != QLatin1String( "lane" ) ) continue;
        const QString target = laneEl.attribute( "target" );
        SAutomationLane *lane = ensureAutomationLane( target );
        if( !lane ) {
            TW_LOGW( "model", "automation: ignoring lane with unknown target '%s'",
                     target.toUtf8().constData() );
            continue;
        }
        lane->readFrom( laneEl );
    }
    // The engine components do not necessarily exist yet on the load path; the
    // owner re-pushes from applyAutomationToEngine() once its chain is up.
    applyAutomationToEngine();
    return 0;
}

void SObject::invalidatePreview()
{
    if( !previewData_ ) return;
    ::free( previewData_ );
    previewData_ = NULL;    
}

int SObject::getPreview( preview_t *dest,
                         offset_t start, length_t length,
                         offset_t nProbes )
{
    // Default preview: if this object renders to a duration, produce peaks from
    // its rendered output. straightCalcPreviewData() reads a random source when
    // there is one (samples) and otherwise reads getRootComponent()'s FROZEN
    // PAGES — so a container (a track/mixer sub-arrangement, i.e. a live asset)
    // is previewable too. No duration -> no preview.
    if( !hasDuration() ) return -1;
    return getStraightPreview( dest, start, length, nProbes );
}

int SObject::straightCalcPreviewData()
{
    if( previewData_ ) return 0;
    if( !hasDuration() ) return -1;    
    previewSkip_ = 256;
    previewForLength_ = getDuration();
    if( !previewForLength_ ) return -2;
    // Create adequate resolution.
    while( previewForLength_<(previewSkip_*128)
           && previewSkip_>0 ) {
        previewSkip_ >>= 1;
    }
    if( !previewSkip_ ) previewSkip_ = 1;
    while( true ) {
        nPreviewProbes_ = previewForLength_ / previewSkip_;
        if( nPreviewProbes_ < 0x200000 ) break;
        previewSkip_ *= 2;        
    }
    // FOr the last, incomplete one.
    nPreviewProbes_++;
    previewData_ = (preview_t *) ::calloc( sizeof( preview_t ), nPreviewProbes_ );
    if( !previewData_ ) return -3;
    qWarning( "SObject::straightCalcPreviewData(): Allocating %d*%d bytes of preview data, "
              "nPreviewProbes_ = %d, previewSkip_ = %d.\n",
              (int)sizeof( preview_t ), (int)nPreviewProbes_,
              (int)nPreviewProbes_, (int)previewSkip_ );
    // Proposal 27: a persisted sidecar with matching geometry satisfies the
    // fill outright — same bytes this loop would compute, minus the reads.
    if( fetchPreviewSidecar( previewData_, nPreviewProbes_,
                             previewSkip_, previewForLength_ ) ) {
        return 0;
    }
    sample_t *buffer = (sample_t *) alloca( previewSkip_ * sizeof( sample_t ) );
    // If this object exposes random-access sample data, read it statelessly:
    // that touches no play cursor and needs no lock, so preview rendering on the
    // UI thread can no longer race playback on the audio thread (proposal 07).
    twRandomSource *rs = getRandomSource();

    // The CONTAINER case (no random source — the object's content is a
    // track/mixer subtree) reads FROZEN PAGES instead. It used to seek the live
    // component and pull calcOutputTo() per preview window, which wrote the
    // shared play cursor from the paint path: the wrong-position-page race
    // twComponent::seek()'s assert was installed to catch. requestPage() takes
    // the freeze path everyone else takes (RenderSession, SCut::buildCapture_),
    // where the component positions ITSELF under its own cursorMutex_.
    //
    // Page granularity is twOutputPage::FRAME_CAPACITY and the pages are chained
    // (page N is handed to page N+1 as previousPage) so stateful DSP resumes
    // instead of restarting — docs/contracts/FREEZE_PROTOCOL.md, "Sequential
    // consumers"; RenderSession's loop is the canonical shape.
    std::shared_ptr<twComponent> rootComp;
    std::shared_ptr<twOutputPage> curPage;
    int srate = 48000;
    if( !rs ) {
        rootComp = getRootComponent();
        if( SProject *proj = getProjectSafe() ) srate = proj->getSRate();
    }
    const offset_t PAGE_FRAMES = (offset_t) twOutputPage::FRAME_CAPACITY;

    // THE CHANNEL FOLD (proposal 36 B8). A preview probe is the signed envelope
    // of its window over ALL CHANNELS: the smallest min and the largest max
    // across every channel, i.e. the union of the per-channel envelopes.
    //
    // It used to be channel 0 alone, which was harmless while nothing above
    // width 1 reached the sink and is a lie now: a clip whose channel 1 carries
    // the loud material would draw as the quiet one, or as silence. A drawn
    // waveform has to describe what is audible.
    //
    // It stays ONE LANE, deliberately, and that is a decision rather than an
    // omission: preview_t, swaveformdraw, SCut::getPreview and every inline
    // renderer are single-envelope by type, an arranger clip lane has no room to
    // stack six waveforms, and a per-channel waveform is a feature (with its own
    // density rules) rather than a fold. The METER is where per-channel level
    // lives; see SLevelMeter.
    //
    // For width 1, and for any file whose channels are identical (test_sawtooth)
    // or nested (test_stereo's 6 dB ladder), the fold produces exactly the bytes
    // channel 0 produced -- which is why no committed waveform moves here. The
    // fold is still key material: twAspect::PreviewPeaksVersion went to 2 so a
    // v1 sidecar written under the channel-0 rule cannot be adopted under this
    // one.

    // Scan [at, at+n) of the CONTAINER's frozen output into the running signed
    // envelope (mn/mx), across every channel of each page. Anything the
    // component cannot produce (no page, short page, past the end) reads as
    // silence, which is what the pulled render produced there too.
    auto scanContainerEnvelope = [&]( offset_t at, offset_t n,
                                      sample_t &mn, sample_t &mx ) {
        offset_t done = 0;
        while( done < n ) {
            const offset_t pos       = at + done;
            const offset_t pageStart = ( pos / PAGE_FRAMES ) * PAGE_FRAMES;
            const offset_t inPage    = pos - pageStart;
            offset_t chunk = n - done;
            if( chunk > PAGE_FRAMES - inPage ) chunk = PAGE_FRAMES - inPage;

            if( !rootComp ) {
                if( mn > 0.f ) mn = 0.f;
                if( mx < 0.f ) mx = 0.f;
                done += chunk;
                continue;
            }
            if( !curPage || curPage->startPosition != pageStart ) {
                // Chain ONLY across a contiguous step; a jump (or the first
                // page) is a discontinuity and must reset, per the freeze
                // protocol's sequential-rendering contract.
                std::shared_ptr<twOutputPage> chainFrom;
                if( curPage
                    && curPage->startPosition + PAGE_FRAMES == pageStart ) {
                    chainFrom = curPage;
                }
                curPage = rootComp->requestPage( pageStart, nullptr, 0, 0,
                                                 srate, chainFrom );
            }
            offset_t avail = curPage
                ? (offset_t) curPage->validFrames - inPage : 0;
            const offset_t pageFrames =
                curPage ? (offset_t) curPage->channelFrames() : 0;
            if( avail > pageFrames - inPage ) avail = pageFrames - inPage;
            if( avail < 0 ) avail = 0;
            if( avail > chunk ) avail = chunk;

            // ACT ON THE WIDTH OF THE PAGE IN HAND (§4.4), never on a declared
            // width: an insert-less twPluginChain forwards its input page
            // verbatim and its silence pages are default-constructed width 1.
            const idx_t nc = curPage ? (idx_t) curPage->channels() : (idx_t) 0;
            for( idx_t c = 0; c < nc; ++c ) {
                const sample_t *d = curPage->channelPtr( c ) + (size_t) inPage;
                for( offset_t k = 0; k < avail; ++k ) {
                    const sample_t a = d[k];
                    if( a < mn ) mn = a;
                    if( a > mx ) mx = a;
                }
            }
            if( avail < chunk || nc == 0 ) {     // the tail reads as silence
                if( mn > 0.f ) mn = 0.f;
                if( mx < 0.f ) mx = 0.f;
            }
            done += chunk;
        }
    };

    const idx_t rsChannels = rs ? rs->channels() : (idx_t) 0;

    // Fill it up.
    for( offset_t i=0; i<(offset_t) previewForLength_; i+=previewSkip_ ) {
	// Yes, this values are other way round. These are the defaults,
	// we want to calc the overall range.
        sample_t min=SAMPLE_NORM_MAX, max = SAMPLE_NORM_MIN;
        if( rs ) {
            // One read per channel, folded into one envelope. twRandomSource is
            // stateless (proposal 07) so repeating the window per channel costs
            // reads and nothing else -- no cursor to displace, unlike the
            // per-channel loop §4.3 forbids on the freeze path.
            for( idx_t c = 0; c < rsChannels; ++c ) {
                rs->read( i, buffer, previewSkip_, c );
                sample_t *p = buffer;
                for( offset_t j=0; j<previewSkip_; ++j ) {
                    sample_t a = *p++;
                    if( a<min ) min = a;
                    if( a>max ) max = a;
                }
            }
            if( rsChannels <= 0 ) { min = 0.f; max = 0.f; }
        } else {
            scanContainerEnvelope( i, previewSkip_, min, max );
        }
	// Now clip to signed 8 bit.
        max = (max*127.) / SAMPLE_NORM_MAX;
        if( max>127. ) max=127.;
        if( max<-128. ) max=-128.;
        min = (min * -127.) / SAMPLE_NORM_MIN;
        if( min>127.) min = 127.;
        if( min<-128.) min = -128.;
        int idx = i/previewSkip_;
        if( idx>=(int)nPreviewProbes_ ) {
            qWarning( "Straight preview store out of range.\n" );
        } else {
            previewData_[i/previewSkip_].min = (char) min;
            previewData_[i/previewSkip_].max = (char) max;
        }
    }
    storePreviewSidecar( previewData_, nPreviewProbes_,
                         previewSkip_, previewForLength_ );
    return 0;
}

bool SObject::fetchPreviewSidecar( preview_t *, offset_t, offset_t, offset_t )
{
    // Default: no sidecar backing — compute as always.
    return false;
}

void SObject::storePreviewSidecar( const preview_t *, offset_t, offset_t, offset_t )
{
}

int SObject::getStraightPreview( preview_t *dest,
                                 offset_t start, length_t length,
                                 offset_t nProbes )
{
    int res;
    length_t myLen;
    if( !hasDuration() ) return -1;
    myLen = getDuration();
    if( !myLen ) return -3;
    if( !previewData_ ) {
        res = straightCalcPreviewData();
        if( res<0 ) {
            qWarning( "Error calculating preview data.\n" );
            return res;
        }
    }
    if( !previewData_ ) {
        qWarning( "Error calculating preview data although clamied he had.\n" );
        return -4;
    }
    // FIXME: Check start and length.
    if( length < 1 ) length = 1;
    for( offset_t i=0; i<nProbes; i++ ) {
        // FIXME: Overflows??? Doubles??
        offset_t realPos = start + ((i*length) / nProbes);
        offset_t probeIdx = realPos/previewSkip_;
        // Clamp probeIdx to valid range [0, nPreviewProbes_-1] to prevent out-of-bounds access
        if( probeIdx >= nPreviewProbes_ ) {
            probeIdx = nPreviewProbes_ - 1;
        }
        preview_t v1 = previewData_[probeIdx];
        *dest++ = v1;
    }
    return 0;
}

bool SObject::hasPreview() const
{
    return false;
}

void SObject::setDuration( length_t )
{
    // FIXME: ENOSYS.
}

int SObject::seekTo( offset_t ofs )
{
    // seek(), not seekTo(): this is the app model crossing into the engine, the
    // definition of an EXTERNAL seek, so it goes through the detector.
    return getRootComponent()->seek( ofs );
}

int SObject::getNReferences() const
{
    return nRefs_;
}

SLink *SObject::childAt( int index ) const
{
    if( index<0 || index>=childOrder_.size() ) return nullptr;
    return childOrder_.at( index );
}

int SObject::indexOfChild( const SLink *child ) const
{
    return childOrder_.indexOf( const_cast<SLink*>( child ) );
}

int SObject::indexOfChildObject( const SObject &child ) const
{
    for( int i=0; i<childOrder_.size(); ++i ) {
        if( &childOrder_.at( i )->getSObject() == &child ) return i;
    }
    return -1;
}

void SObject::moveChildToIndex( int fromIndex, int toIndex )
{
    const int n = childOrder_.size();
    if( fromIndex<0 || fromIndex>=n ) return;
    if( toIndex<0 ) toIndex = 0;
    if( toIndex>=n ) toIndex = n-1;
    if( fromIndex==toIndex ) return;
    // Order is just the explicit list; QObject parentage is unaffected.
    childOrder_.move( fromIndex, toIndex );
}

void SObject::addRef()
{
    // nRefs_ is a plain int and add/removeRef emit signals: lifetime is
    // main-thread-only by design (THREADING.md Rule 1). Workers must go
    // through the IRevalidatable seams (revalAddRef/revalRemoveRef), never
    // through refcounts.
    Q_ASSERT( QThread::currentThread() == thread() );
    deletePending_.store( false );   // a new reference rescinds a queued death
    if( ++nRefs_ == 1 ) {
        emit gotReferenced();
    }
    emit nRefsChanged();
}

void SObject::removeRef()
{
    Q_ASSERT( QThread::currentThread() == thread() );
    if( nRefs_==0 ) {
        qWarning( "SObject::removeRef(): Called although reference count was zero.\n" );
        return;
    }
    if( (--nRefs_)==0 ) {
        emit gotUnreferenced();
    }
    emit nRefsChanged();
    if( 0==nRefs_ ) {
        // This will delete the object if the application reenters the main
        // loop. deleteLater() cannot be rescinded — SObject::event() swallows
        // the deferred delete if the object gets re-referenced (or is pinned
        // by the revalidator) before then; deletePending_ lets the last unpin
        // re-arm it.
        deletePending_.store( true );
        deleteLater();
    }
}

// Revalidator keep-alive pins. Called from BOTH the main thread
// (scheduleRevalidation on an edit) and the worker pool (re-queues and every
// job-exit path) — hence a separate atomic, never the Qt refcount above:
// racing the non-atomic nRefs_ corrupted it, and a premature deleteLater()
// destroyed an object that live SLinks still referenced (the vtable-garbage
// paint crash after a split).
void SObject::revalAddRef()
{
    revalPins_.fetch_add( 1 );
}

void SObject::revalRemoveRef()
{
    if( revalPins_.fetch_sub( 1 ) == 1 ) {
        // Last pin released. If a refcount-driven deletion arrived while we
        // were pinned, event() swallowed it — re-arm it now. deleteLater() is
        // thread-safe; event() re-checks everything on the object's thread,
        // so a stale or duplicate post is harmless.
        if( deletePending_.load() ) {
            deleteLater();
        }
    }
}

/**
 * This is a simple method to scan through all children.
 * Every link has a start time, every object (maybe) a duration.
 * We assume, that all of my children belong to my events.
 */
offset_t SObject::getChildrenExtent( offset_t &firstStart, offset_t &lastEnd,
                                     int &nUndefStart, int &nUndefDuration ) const
{
    nUndefStart = 0;
    nUndefDuration = 0;
    firstStart = (offset_t) (0-1); // Largest number possible
    lastEnd = (offset_t) 0;

    const auto& links = childLinks();
    if( links.isEmpty() ) {
        return 0;
    }

    for( SLink *sli : links ) {
        if( !sli ) {
            ++nUndefStart;
            ++nUndefDuration;
            continue;
        }

        // Safely get the child object; skip if invalid
        SObject *sobj_ptr = nullptr;
        try {
            sobj_ptr = &sli->getSObject();
        } catch (...) {
            ++nUndefStart;
            ++nUndefDuration;
            continue;
        }

        if( !sobj_ptr ) {
            ++nUndefStart;
            ++nUndefDuration;
            continue;
        }

        bool hd = false;
        try {
            hd = sobj_ptr->hasDuration();
        } catch (...) {
            hd = false;
        }

        if( sli->hasStartTime() ) {
            offset_t st = sli->getStartTime();
            if( st<firstStart ) firstStart = st;
            if( hd ) {
                try {
                    offset_t du = (offset_t) sobj_ptr->getDuration();
		    // qWarning( "SObject::getChildrenExtent(): Duration = %d:%d.",
		    //	  (int)(du>>32),(int)du );
                    du += st;
		    // qWarning( "SObject::getChildrenExtent(): Duration = %d:%d.",
		    //	  (int)(du>>32),(int)du );
                    if( du>lastEnd ) lastEnd = du;
                } catch (...) {
                    // Ignore errors in getting duration
                }
            }
        } else {
            ++nUndefStart;
        }
        if( !hd ) {
            ++nUndefDuration;
        }
    }
    return lastEnd-firstStart;
}

bool SObject::hasDuration() const
{
    offset_t first, last;
    int nUndefStart, nUndefDuration;
    getChildrenExtent( first, last, nUndefStart, nUndefDuration );
    return nUndefStart==0 && nUndefDuration==0;
}

offset_t SObject::getFirstChildStartTime() const
{
    offset_t first, last;
    int nUndefStart, nUndefDuration;
    getChildrenExtent( first, last, nUndefStart, nUndefDuration );
    return first;
}    

length_t SObject::getDuration() const
{
    return getAllChildsDuration();
}    

length_t SObject::getAllChildsDuration() const
{
    offset_t first, last;
    int nUndefStart, nUndefDuration;
    getChildrenExtent( first, last, nUndefStart, nUndefDuration );
    return last-first;
}    

bool SObject::isEmpty() const
{
    return childOrder_.isEmpty();
}

void SObject::childEvent( QChildEvent *ce )
{
    // The child tree is main-thread-only (THREADING.md); parentage changes off
    // the main thread would race every unlocked childOrder_ iteration.
    Q_ASSERT( QThread::currentThread() == thread() );
    QObject::childEvent( ce );
    // Keep childOrder_ membership in sync with QObject parentage. New children
    // append (order is then adjusted via moveChildToIndex); removed children
    // drop out. Mirror QObject's own list state: at ChildAdded the child is
    // already in children(); at ChildRemoved it is already gone.
    if( ce->added() ) {
        // Only SLinks are placements. The old blind (SLink*) cast put ANY
        // QObject child into childOrder_, where every iterator dereferences
        // it as a link (type-confusion crash). qobject_cast also rejects a
        // constructed-with-parent SLink (still a plain QObject at ChildAdded
        // time) — that is the slink.h construction rule, now enforced.
        SLink *lk = qobject_cast<SLink *>( ce->child() );
        if( !lk ) return;
        if( !childOrder_.contains( lk ) ) childOrder_.append( lk );
        gotChild( *lk );
    } else if( ce->removed() ) {
        // The child may be mid-destruction — compare by pointer value only.
        // (Dereferencing it as an SLink in lostChild() is safe only because
        // ~SLink detaches while still fully typed; anything that was never in
        // childOrder_ — foreign child or rule-violating link — is skipped.)
        SLink *lk = static_cast<SLink *>( (QObject *)ce->child() );
        if( childOrder_.removeOne( lk ) ) {
            lostChild( *lk );
        }
    }
}

// DeferredDelete is the model's GC tick (removeRef() -> deleteLater()). It
// cannot be rescinded, so an object re-referenced after its refcount touched
// zero would still be destroyed here — with live SLinks pointing at it.
// Swallow the stale deletion instead; a later drop to zero posts a fresh one.
bool SObject::event( QEvent *e )
{
    if( e->type() == QEvent::DeferredDelete ) {
        if( nRefs_ > 0 ) {
            // 1->0->1 resurrection: something re-linked this object after its
            // refcount touched zero. Dropping the stale deletion here is what
            // makes that pattern safe.
            qWarning( "SObject::event(): '%s' resurrected after a queued "
                      "deletion (refcount %d) — deferred delete ignored.",
                      qPrintable( sName_ ), nRefs_ );
            return true;
        }
        if( revalPins_.load() > 0 ) {
            // A reval job still holds a borrowed pointer to us; deletePending_
            // is set, so the last revalRemoveRef() re-arms the deletion.
            return true;
        }
    }
    return QObject::event( e );
}

void SObject::gotChild( SLink &newChild )
{
    emit childObjectAdded( newChild );
}

void SObject::lostChild( SLink &newChild )
{
    emit childObjectRemoved( newChild );
}

int SObject::getChildIndex( SObject &child ) const
{
    return indexOfChildObject( child );

}

SObject::SObject( SProject *project )
    : QObject( project ),
      nRefs_( 0 ),
      previewForLength_( 0 ),
      nPreviewProbes_( 0 ),
      previewData_( 0 ),
      previewSkip_( 0 ),
      solo_( false ),
      muted_( false ),
      armed_( false ),
      volume_( 0.0 ),
      pan_( 0.0 ),
      delay_( 0.0 ),
      sName_( DEFAULT_SNAME )
{
    // We neither want to remember  previews if we have changed our duration
    // (Although we could reimplement it for that special case)
    // nor for the being unreferenced (just wastes memory).
    QObject::connect( this, SIGNAL( durationChanged( length_t ) ),
                      this, SLOT( invalidatePreview() ) );
    QObject::connect( this, SIGNAL( gotUnreferenced() ),
                      this, SLOT( invalidatePreview() ) );
}

SObject::~SObject()
{
    // Diagnostic for the vtable-garbage-at-paint crash class: an SObject must
    // never die while SLinks still reference it (each live link holds a ref;
    // their ~SLink drops it before we can get here). If this fires, the NEXT
    // dereference of such a link is a use-after-free — this warning names the
    // culprit at the moment of the actual bug instead. (Project teardown
    // deletes links before objects, so a clean shutdown stays silent.)
    if( nRefs_ > 0 ) {
        qWarning( "SObject::~SObject(): '%s' destroyed with %d live "
                  "reference(s) — a referencing SLink now dangles!",
                  qPrintable( sName_ ), nRefs_ );
    }
}

SProject *SObject::getProjectSafe() const
{
    return dynamic_cast<SProject*>( parent() );
}

// Phase 5e.6: revalidator worker → queued UI repaint. Runs on a worker thread
// with no locks held; the queued invocation delivers on the project's (UI)
// thread, where views connected to captureRevalidated() repaint and re-pull
// the now-valid capture.
void SObject::revalCompleted(uint32_t aspects)
{
    if (!(aspects & (Preview | Metadata))) return;
    SProject *project = getProjectSafe();
    if (!project) return;
    QMetaObject::invokeMethod(project, "notifyCaptureRevalidated",
                              Qt::QueuedConnection);
}

// --- Scoped render-cache invalidation (proposal 15) ------------------------

// Depth-first containment walk. Bumps AFTER recursing so the deepest chains
// go stale first; harmless either way (staleness is checked per producer),
// but it mirrors the render pull order. Multiple links to the same object
// bump it more than once — a spare atomic increment, not a problem.
bool SObject::invalidateRenderChainsContaining(SObject *target)
{
    bool contains = (this == target);
    for (SLink *lk : childLinks()) {
        if (!lk) continue;
        if (lk->getSObject().invalidateRenderChainsContaining(target))
            contains = true;
    }
    if (contains)
        bumpRenderChainEpoch();
    return contains;
}

void SObject::invalidateRenderPath()
{
    SProject *project = getProjectSafe();
    SObject *root = project ? project->getRootComponent() : nullptr;
    if (root) {
        root->invalidateRenderChainsContaining(this);
    } else {
        // Not (yet) reachable from a project root — e.g. during construction
        // or teardown. Our own caches are all we can reach; when this object
        // is later linked into the tree, the container's child-added path
        // invalidates the ancestors.
        bumpRenderChainEpoch();
    }
}

// --- Range-scoped invalidation (proposal 18 Phase 5) ------------------------

// Positions saturate at INT64_MAX: unbounded clip extents use UINT64_MAX,
// and downstream mapping (SCut's exact rational math) works in int64.
static offset_t satShift(offset_t pos, offset_t shift)
{
    const offset_t CAP = (offset_t) INT64_MAX;
    if (pos >= CAP - shift) return CAP;
    return pos + shift;
}

QList<SObject::SDirtyRange> SObject::mapChildRangesToSelf(
    SLink *childLink, const QList<SDirtyRange> &childRanges )
{
    // Default containment mapping: the child's timeline is shifted into
    // ours by the link's start time (the ShiftMap of proposal 18).
    offset_t shift = (childLink && childLink->hasStartTime())
                   ? childLink->getStartTime() : 0;
    QList<SDirtyRange> out;
    for (const SDirtyRange &r : childRanges)
        out.append({ satShift(r.start, shift), satShift(r.end, shift) });
    return out;
}

// Same depth-first containment walk as invalidateRenderChainsContaining,
// carrying the dirty ranges upward. Each level bumps its OWN chain only over
// the ranges mapped into its domain — a windowed-away edit (empty ranges)
// bumps nothing on that branch even though the branch contains the target.
bool SObject::invalidateRenderChainsContainingRange(
    SObject *target, offset_t targetStart, offset_t targetEnd,
    QList<SDirtyRange> &rangesInSelf )
{
    bool contains = (this == target);
    if (contains)
        rangesInSelf.append({ targetStart, targetEnd });
    for (SLink *lk : childLinks()) {
        if (!lk) continue;
        QList<SDirtyRange> childRanges;
        if (lk->getSObject().invalidateRenderChainsContainingRange(
                target, targetStart, targetEnd, childRanges)) {
            contains = true;
            rangesInSelf.append( mapChildRangesToSelf(lk, childRanges) );
        }
    }
    if (contains) {
        for (const SDirtyRange &r : rangesInSelf)
            if (r.end > r.start)
                bumpRenderChainEpochRange(r.start, r.end);
    }
    return contains;
}

void SObject::invalidateRenderPathRange( offset_t start, offset_t end )
{
    if (end <= start) return;
    SProject *project = getProjectSafe();
    SObject *root = project ? project->getRootComponent() : nullptr;
    if (root) {
        QList<SDirtyRange> ranges;
        root->invalidateRenderChainsContainingRange(this, start, end, ranges);
    } else {
        bumpRenderChainEpoch();
    }
}

// Register a dependent link (object that references this one).
// Called when a cut or asset placement references this object.
// Uses SLink (the native way to track references) for dependency tracking.
// Connects to the link's destroyed signal to auto-unregister if the link is deleted.
void SObject::addDependentLink(SLink *dependentLink)
{
    if (!dependentLink) return;
    {
        std::lock_guard<std::mutex> lock(dependentsMutex_);
        dependentLinks_.insert(dependentLink);
    }

    // Auto-unregister this link when it's destroyed (proposal 06: safe cleanup).
    // If the cut/link is deleted, remove it from our dependent set to avoid
    // stale pointers. Use a lambda to capture 'this' and the link safely.
    QObject::connect(dependentLink, &QObject::destroyed,
                     this, [this, dependentLink]() {
        removeDependentLink(dependentLink);
    });
}

// Unregister a dependent link. Called when a reference is removed.
void SObject::removeDependentLink(SLink *dependentLink)
{
    if (!dependentLink) return;
    std::lock_guard<std::mutex> lock(dependentsMutex_);
    dependentLinks_.remove(dependentLink);
}

// Notify dependent links that specific aspects have changed.
// Only affected dependents are invalidated (lazy invalidation).
// Example: setMuted() → notifyDependentsChanged(Playback | Metadata)
//
// This allows fine-grained invalidation: muting a track only invalidates
// the Playback aspects of cuts that capture that track's output, not the
// entire arrangement or unrelated clips.
//
// NOTE: Links are auto-unregistered on destruction via the destroyed() signal
// connected in addDependentLink(). This is safe even if a link is destroyed
// while we're iterating (we collect links under lock first).
void SObject::notifyDependentsChanged(uint32_t affectedAspects)
{
    // Collect dependents under lock, then notify without holding lock.
    // This snapshot approach is safe: even if a link is destroyed during iteration,
    // the destroyed() signal will trigger removeDependentLink(), but we're working
    // on our own snapshot, not the shared set.
    QSet<SLink*> dependents;
    {
        std::lock_guard<std::mutex> lock(dependentsMutex_);
        dependents = dependentLinks_;
    }

    // Notify each dependent (typically a cut) to invalidate affected aspects.
    // Virtual dispatch: the base no-op covers objects without captures, so
    // this file needs no knowledge of concrete types.
    for (SLink *link : dependents) {
        if (!link) continue;  // Defensive: skip if somehow null (shouldn't happen)
        link->getSObject().invalidateAspects(affectedAspects);
    }
}

// Phase 5e: Page cache API
// Base implementation: just returns current page without scheduling.
// Derived classes (SCut, STrack, etc.) override to call scheduleRevalidation().
double SObject::volumeDbSnapshot() const
{
    std::lock_guard<std::mutex> lock( volumeMutex_ );
    return getVolume();
}

std::shared_ptr<CapturePageData> SObject::getCapture(uint32_t aspectsMask)
{
    if (aspectsMask == 0) return nullptr;

    // Get current page (may be stale, may be null, never blocks).
    // Read current page without locking (shared_ptr copy is atomic).
    auto page = currentPage();

    // If current page has all needed aspects, return immediately.
    // Acquire page lock to safely read validAspects (prevents torn reads during concurrent writes).
    if (page) {
        std::lock_guard<std::mutex> pageLock(page->pageMutex);
        if ((page->validAspects & aspectsMask) == aspectsMask) {
            return page;
        }
    }

    // TODO: Phase 5e.5 - Unify CaptureRevalidator to work with SObject*.
    // For now, derived classes (SCut) override this to call scheduleRevalidation().

    // Return current page anyway (stale is OK; better than null/dropout)
    return page;
}

bool SObject::needsRevalidation_nolock(uint32_t aspectsMask) const
{
    if (aspectsMask == 0) return false;
    // Note: _nolock refers to not holding SObject's state mutex; we still need page lock for valid aspect check
    if (!currentPage_) return true;
    std::lock_guard<std::mutex> pageLock(currentPage_->pageMutex);
    return (currentPage_->validAspects & aspectsMask) != aspectsMask;
}
