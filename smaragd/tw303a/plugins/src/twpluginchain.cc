#include "tw/plugins/twpluginchain.h"

#include "tw/plugins/twplugininsert.h"
#include "tw/pages/io_vector.h"

#include <algorithm>
#include <vector>

twPluginChain::twPluginChain( tw303aEnvironment &env, idx_t nBusses )
    : twComponent( env ), nBusses_( nBusses )
{
    // allocPlugs() and createOutputLatches() are called by init()
}

twPluginChain::~twPluginChain() = default;

void twPluginChain::createOutputLatches()
{
    pOutputLatches_.resize(nBusses_);
    for( idx_t i = 0; i < nBusses_; ++i )
        pOutputLatches_[i] = std::make_shared<twStreamingLatch>( shared_from_this(), i, 4096 );
}

// Snapshot the ordered insert list. Every path below works on the snapshot and
// releases pluginsMutex_ first, so no upstream render ever runs with it held —
// the same discipline teardown() already used.
std::vector<std::shared_ptr<twComponent> > twPluginChain::snapshotPlugins() const
{
    std::lock_guard<std::mutex> lock( pluginsMutex_ );
    return plugins_;
}

// Phase 3: IOVector-based interface (type-safe, page-backed rendering)
length_t twPluginChain::calcOutputTo( IOVector& dest, idx_t port )
{
    // Fast path: Check if component is being torn down
    if (state_.load(std::memory_order_acquire) == ComponentState::ZOMBIE) {
        return dest.fillSilence(0, dest.length());
    }

    // Holding pluginsMutex_ across the whole pull — which reaches every insert
    // and, through them, the upstream graph — is only safe because the REALTIME
    // AUDIO CALLBACK NEVER RENDERS: twRtThreadGuard makes freezePage() on the RT
    // thread a reported error, and the RT path reads ready pages with the
    // proposal-16 stale fallback instead. If that ever changes, this lock
    // becomes a priority-inversion hazard on the audio thread and must go.
    std::lock_guard<std::mutex> lock( pluginsMutex_ );

    if( plugins_.empty() ) {
        // No plugins: passthrough from input via IOVector
        if( port < (idx_t)pInputPlugs_.size() && pInputPlugs_[port] ) {
            std::vector<sample_t> buffer(dest.length());
            auto *input = static_cast<twLatchStreamingOutput *>(pInputPlugs_[port].get());
            length_t len = input->readStreamingData( buffer.data(), dest.length() );
            return dest.copyFrom(IOVector::CreateFromBuffer(buffer.data(), len), 0, len);
        }
        return dest.fillSilence(0, dest.length());
    }

    // New block: reset all inserts so each processes fresh audio this callback.
    for( std::shared_ptr<twComponent> plugin : plugins_ )
        std::static_pointer_cast<audio::twPluginInsert>(plugin)->resetBlock();

    // Pull from the last insert's output (wiring established in rebuildWiring).
    // Every insert is a 1-in/1-out bus tap, so port is always 0 here: this chain
    // IS one bus (STrack builds one twPluginChain per bus).
    std::shared_ptr<twComponent> lastPlugin = plugins_.back();
    if( lastPlugin && port < lastPlugin->getNOutputs() ) {
        return lastPlugin->calcOutputTo( dest, port );
    }

    return dest.fillSilence(0, dest.length());
}

void twPluginChain::addPlugin( std::shared_ptr<audio::twPluginInsert> insert )
{
    if( insert ) {
        std::lock_guard<std::mutex> lock( pluginsMutex_ );
        plugins_.push_back( std::static_pointer_cast<twComponent>(insert) );
        rebuildWiring_nolock();
    }
}

void twPluginChain::removePlugin( int index )
{
    std::lock_guard<std::mutex> lock( pluginsMutex_ );
    if( index >= 0 && index < (int)plugins_.size() ) {
        plugins_.erase( plugins_.begin() + index );
        rebuildWiring_nolock();
    }
}

void twPluginChain::removePlugin( const std::shared_ptr<audio::twPluginInsert> &insert )
{
    if( !insert ) return;
    std::lock_guard<std::mutex> lock( pluginsMutex_ );
    // Compare the pointed-to object, not the shared_ptr: plugins_ holds
    // twComponent bases and the caller holds the twPluginInsert derived type,
    // so the two shared_ptrs are different types over the same control block.
    const twComponent *needle = static_cast<const twComponent *>( insert.get() );
    auto it = std::find_if( plugins_.begin(), plugins_.end(),
                            [needle]( const std::shared_ptr<twComponent> &p ) {
                                return p.get() == needle;
                            } );
    if( it != plugins_.end() ) {
        plugins_.erase( it );
        // _nolock: pluginsMutex_ is NOT recursive and we are holding it.
        rebuildWiring_nolock();
    }
}

void twPluginChain::reorderPlugin( int fromIndex, int toIndex )
{
    std::lock_guard<std::mutex> lock( pluginsMutex_ );
    if( fromIndex < 0 || fromIndex >= (int)plugins_.size() ||
        toIndex < 0 || toIndex >= (int)plugins_.size() ||
        fromIndex == toIndex ) {
        return;
    }
    auto plugin = plugins_[fromIndex];
    plugins_.erase( plugins_.begin() + fromIndex );
    plugins_.insert( plugins_.begin() + toIndex, plugin );
    rebuildWiring_nolock();
}

int twPluginChain::seekTo( offset_t offset )
{
    std::vector<std::shared_ptr<twComponent> > snapshot = snapshotPlugins();

    // Forward the seek to this chain's own inserts, and NO further. Each tap
    // only records the position (twPluginInsert::seekTo stores renderPos_),
    // which is how renderFrames() learns the page position its signature does
    // not carry — a purely local, freeze-scoped write.
    for( std::shared_ptr<twComponent> plugin : snapshot ) {
        if( plugin ) {
            plugin->seekTo( offset );
        }
    }

    // The chain deliberately does NOT seek its input plugs' PRODUCERS (the
    // track mix feeding it) any more. Every insert reads its producer through
    // requestPage(startPos, ...), which is positional by construction (see the
    // rationale at twPluginInsert::seekTo), so moving the producer's cursor
    // from here bought nothing — and could only race the producer's own
    // freeze, which seeks under a DIFFERENT lock (cursorMutex_) than a seek
    // takes (mutex()). A seek that lands between a freeze's
    // seekTo(page->startPosition) and its renderFrames() displaces that whole
    // page's content while it is still cached under, and served for, the
    // original position.

    return 0;
}

void twPluginChain::rebuildWiring()
{
    std::lock_guard<std::mutex> lock( pluginsMutex_ );
    rebuildWiring_nolock();
}

// Caller must hold pluginsMutex_.
void twPluginChain::rebuildWiring_nolock()
{
    // Set up wiring: input → insert0 → insert1 → ... → output.
    // Called when inserts are added/removed/reordered, not per sample block.
    //
    // Since proposal 08 M3 every insert is a 1-in/1-out BUS TAP and every chain
    // serves exactly one bus, so this is a plain series wiring of port 0. The
    // pre-M3 loop over `port < nBusses_` was the bug that fed a 2-in plugin bus
    // audio on input 0 and SILENCE on input 1: a chain built with nBusses_ == 1
    // could only ever wire one of the plugin's two inputs. Channel coherence is
    // now the shared twPluginSlotProcessor's job, which gathers every bus
    // through the sibling taps.

    if( plugins_.empty() ) {
        return;
    }

    for( idx_t port = 0; port < nBusses_; ++port ) {
        // First insert gets the track input
        std::shared_ptr<twComponent> firstPlugin = plugins_[0];
        if( firstPlugin && port < firstPlugin->getNInputs() && port < (idx_t)pInputPlugs_.size() && pInputPlugs_[port] ) {
            // Give the head tap its OWN output from the producing latch — do NOT
            // hand it pInputPlugs_[port], which is OUR plug. Sharing one
            // twLatchOutput between two consumers is a trap: whoever disconnects
            // first calls twLatch::deleteOutput() on it, which drops it from the
            // latch's outputList for BOTH. Our shared_ptr keeps the object alive
            // so the pointer still looks valid, but sharedOutput() can no longer
            // vend it, so setInput() silently leaves the input NULL and the whole
            // chain goes silent. That is exactly what happened when the FIRST
            // insert was removed: the departing tap took the chain's input with
            // it. setInput() deletes the tap's previous plug, so re-running this
            // does not accumulate outputs.
            twLatch &producer = pInputPlugs_[port]->getParentLatch();
            if( twLatchOutput *ownPlug = producer.addOutput() ) {
                firstPlugin->setInput( port, ownPlug );
            }
        }

        // Wire each subsequent insert to the previous one's output
        for( size_t i = 1; i < plugins_.size(); ++i ) {
            std::shared_ptr<twComponent> prevPlugin = plugins_[i-1];
            std::shared_ptr<twComponent> nextPlugin = plugins_[i];
            if( prevPlugin && nextPlugin &&
                port < prevPlugin->getNOutputs() && port < nextPlugin->getNInputs() ) {
                auto* prevOutput = prevPlugin->linkOutput( port );
                if( prevOutput ) {
                    nextPlugin->setInput( port, prevOutput );
                }
            }
        }
    }
}


std::shared_ptr<twOutputPage> twPluginChain::freezePage(
    offset_t startPos,
    const sample_t *inputData,
    uint64_t inputOffset,
    length_t inputLength,
    int sampleRate,
    std::shared_ptr<twOutputPage> previousPage )
{
    if (state_.load(std::memory_order_acquire) == ComponentState::ZOMBIE) {
        auto silencePage = std::make_shared<twOutputPage>();
        silencePage->setValidFrames(0);
        return silencePage;
    }

    // Snapshot, then release: the pull below reaches the whole upstream graph,
    // and pluginsMutex_ is also taken by the UI thread's addPlugin/removePlugin
    // and by bumpContentEpoch().
    std::vector<std::shared_ptr<twComponent> > snapshot = snapshotPlugins();

    std::shared_ptr<twLatchOutput> plug0;
    {
        std::lock_guard<std::mutex> lock( mutex() );
        if( !pInputPlugs_.empty() ) plug0 = pInputPlugs_[0];
    }

    // No inserts: forward the track mix's page unchanged. requestPage(), not
    // freezePage(), so two drivers demanding the same page collapse to one
    // render (proposal 19 Phase 2a).
    if( snapshot.empty() ) {
        if( plug0 ) {
            std::shared_ptr<twComponent> comp = plug0->getParentLatch().getComponent();
            if( comp )
                return comp->requestPage( startPos, inputData, inputOffset,
                                          inputLength, sampleRate, previousPage );
        }
        auto silencePage = std::make_shared<twOutputPage>();
        silencePage->setValidFrames(0);
        return silencePage;
    }

    // With inserts: the chain no longer threads pages through the list itself.
    // Each tap is wired to its predecessor (rebuildWiring) and pulls its own
    // upstream, so asking the LAST tap for the page walks the whole chain — and
    // does it through requestPage(), the dedup front door, instead of the
    // hand-rolled recursion the pre-M3 code used (which also mis-used the
    // previousPage argument as "the input page", conflating a temporal
    // predecessor with an upstream producer).
    //
    // previousPage is OUR predecessor, not the tap's, so it is not forwarded:
    // the taps track page continuity through the processor's own position
    // bookkeeping.
    std::shared_ptr<twComponent> last = snapshot.back();
    if( !last ) {
        auto silencePage = std::make_shared<twOutputPage>();
        silencePage->setValidFrames(0);
        return silencePage;
    }

    std::shared_ptr<twOutputPage> page = last->requestPage(
        startPos, nullptr, 0, inputLength, sampleRate, nullptr );
    if( !page ) {
        page = std::make_shared<twOutputPage>();
        page->setValidFrames(0);
    }
    return page;
}

void twPluginChain::reset()
{
    // Stateless component: inserts (and their shared processors) hold the state.
}

void twPluginChain::bumpContentEpoch()
{
    twComponent::bumpContentEpoch();

    // Insert pages carry rendered upstream audio; stale them along with us.
    for( const std::shared_ptr<twComponent> &plugin : snapshotPlugins() ) {
        if (plugin) plugin->bumpContentEpoch();
    }
}

void twPluginChain::invalidatePagesInRange(offset_t start, offset_t end)
{
    twComponent::invalidatePagesInRange(start, end);

    // Same forwarding as bumpContentEpoch: insert pages bake in upstream
    // audio, so the affected range stales them too (positions are shared -
    // the whole chain speaks absolute timeline frames).
    for( const std::shared_ptr<twComponent> &plugin : snapshotPlugins() ) {
        if (plugin) plugin->invalidatePagesInRange(start, end);
    }
}

void twPluginChain::teardown()
{
    state_.store(ComponentState::ZOMBIE, std::memory_order_release);

    if (auto parent = parentComponent_.lock()) {
        if (myInputIndex_ >= 0) {
            parent->removeInput(myInputIndex_);
        }
    }

    std::vector<std::shared_ptr<twComponent> > depsCopy;
    {
        std::lock_guard<std::mutex> lock(mutex());
        depsCopy = dependents_;
    }
    for (auto dep : depsCopy) {
        if (dep) dep->onDependencyTeardown(shared_from_this());
    }

    // Tear down each insert in order (snapshot, don't hold the lock during it)
    std::vector<std::shared_ptr<twComponent> > pluginsCopy;
    {
        std::lock_guard<std::mutex> lock(pluginsMutex_);
        pluginsCopy = plugins_;
        plugins_.clear();
    }

    for (auto plugin : pluginsCopy) {
        if (plugin) plugin->teardown();
    }
}

void twPluginChain::onDependencyTeardown(std::shared_ptr<twComponent> dep)
{
    // An insert is being torn down; remove it from our list
    std::lock_guard<std::mutex> lock(pluginsMutex_);
    auto it = std::find_if(plugins_.begin(), plugins_.end(),
        [dep](std::shared_ptr<twComponent> p) {
            return p == dep;
        });
    if (it != plugins_.end()) {
        plugins_.erase(it);
        rebuildWiring_nolock();
    }
}
