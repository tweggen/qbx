

#include <QTimer>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

#include "tw/graph/tw303aenv.h"
#include "tw/core/twlog.h"
#include "tw/plugins/twplugindescriptor.h"
#include "tw/plugins/twpluginsearchpaths.h"
#include "tw/sidecar/twsidecarstore.h"
#include "tw/devices/audio_backend.h"
#include "tw/devices/keyboard_midi.h"
#include "tw/playback/twspeaker.h"
#include "tw/schedule/capture_revalidator.h"
#include "tw/dsp/twwhitenoise.h"
#include "tw/dsp/twconstant.h"

#include "app/model/sobject.h"
#include "app/model/slink.h"
#include "app/shell/sapplication.h"
#include "app/model/sproject.h"
#include "app/shell/ssettings.h"
#include "app/shell/smidiinputhub.h"
#include "app/shell/smidioutpump.h"
#include "app/shell/slivemonitor.h"
#include "app/shell/sautomationrecorder.h"
#include "app/servicesui/soptions.h"
#include "app/actions/sactionhistory.h"
#include "app/actions/saction.h"
#include "app/selection/sselectionmanager.h"
#include "app/selection/ssetselectionaction.h"
#include "app/selection/saddtoselectionaction.h"
#include "app/selection/sremovefromselectionaction.h"
#include "app/selection/sclearselectionaction.h"
#include "app/selection/stoggleselectionaction.h"
#include "app/objects/track/sinstrumenttracks.h"
#include "app/objects/track/strack.h"
#include "app/objects/track/spluginslot.h"

#include <cmath>
#include <limits>
#include <vector>

SApplication *SApplication::singleton_ = NULL;

SProject *SApplication::getCurrentProject() const
{
    return currentProject_;
}

/**
 * Change modality for a new project.
 * Here we currently:
 * - Rewire the main output speaker for a new project.
 */
void SApplication::setCurrentProject( SProject *cp )
{
    currentProject_ = cp;
    // Push the project's sample rate / candidate set into the engine. For a
    // loaded project this runs again after the loader has populated these from
    // XML (see SMainWindow::fileOpen), so the engine ends up on the saved rate.
    if( cp && t3Env_ ) {
        t3Env_->setSRate( cp->getSRate() );
        t3Env_->setCandidateRates( cp->candidateRates() );
    }
    // Stage 5: the playback readahead consumes the project's page scheduler
    // (demands instead of pulling freezes). Null (no project / workers=0)
    // keeps the legacy pull. Takes effect on the next startOutput().
    if( t3Speaker_ )
        t3Speaker_->setPageScheduler( cp ? cp->getRevalidator() : nullptr );
    rewireSpeaker();
    // Every track that arrived ARMED out of a project file is inert until the
    // user arms it in this session (design D9): loading a project must not open
    // the microphone.
    if( liveMonitor_ ) liveMonitor_->projectChanged();
}

void SApplication::rewireSpeaker()
{
    // Drop any prior wiring first.
    getSpeaker()->setInput( 0, NULL );
    if( !currentProject_ || !currentProject_->getRootComponent() ) return;

    // ONE plug (proposal 36 B5). There used to be a second, wired to
    // root->linkOutput(1) when the root reported more than one output, from the
    // days when a track was N parallel mono wires. Since B4 the graph is one
    // wire N channels wide, and the speaker's plug carries no audio in any case
    // — it supplies the WIRE FORMAT that startOutput() opens the device with.
    //
    // twRewire (the rewire root inside SStdMixer) returns NULL from
    // linkOutput() when nothing has been wired into it yet, so this call
    // is only meaningful once the graph has at least one track.
    std::shared_ptr<twComponent> root = currentProject_->getRootComponent()->getRootComponent();
    getSpeaker()->setInput( 0, root->linkOutput( 0 ) );
}

bool SApplication::isPlaying() const
{
    return isPlaying_;
}

void SApplication::setStatusMode( const QString &mode )
{
    if( mode == statusMode_ ) return;
    statusMode_ = mode;
    emit statusModeChanged( statusMode_ );
}

void SApplication::setPlaying( bool f )
{
    isPlaying_ = f;
    // Run the playhead poll while playing; on stop, pumpLocator() emits one final
    // update and self-stops the timer (unless recording is still driving it).
    if( locatorTimer_ ) {
        if( f ) { if( !locatorTimer_->isActive() ) locatorTimer_->start(); }
        else pumpLocator();
    }
    // Meters: start on play; on stop leave the pump running so the bars decay to
    // the floor (pumpMeters counts the tail down and stops itself).
    if( f ) startMetering();

    // MIDI out (proposal 37 P7b). NOT a fold into the metering pump: the meters
    // deliberately keep ticking after a stop so the bars can decay, whereas
    // MIDI-out must go silent - and send its all-notes-off - at the instant the
    // transport does. A render never gets here (it does not set isPlaying_),
    // which is what makes "renders emit nothing" true by construction.
    if( midiOutPump_ ) {
        if( f ) midiOutPump_->start();
        else    midiOutPump_->stop();
    }

    // Automation write passes (proposal 37 P6) are bounded by the TRANSPORT:
    // Write's overwrite window opens where the run did, and Latch holds its
    // last value until the run ends. A stop therefore commits whatever pass is
    // open - one action, here, on the main thread.
    if( automationRecorder_ ) {
        if( f ) automationRecorder_->transportStarted( getGlobalLocatorPos() );
        else    automationRecorder_->transportStopped( getGlobalLocatorPos() );
    }

    // The live lane (proposal 21 L1b). The transport decides the FEED POLICY
    // and the automation hold (design D2), and under monitor Auto it decides
    // whether there is a live lane at all - tape style, the input gives way to
    // the track's own material on plain Play. So a transport edge is a plan
    // REBUILD plus exactly one explicit reposition.
    if( liveMonitor_ ) liveMonitor_->transportChanged();
}

const SSelectionList &SApplication::getSelectionList() const
{
    return *selectionList_;
}

bool SApplication::isSelectionEmpty() const
{
    return selectionList_->count()==0;
}

bool SApplication::isSLinkSelected( SLink *lk ) const
{
    return selectionList_->contains( lk )>0; 
}

/**
 * Add the given slink to the selection list, if not already there.
 * If this is the first link selected, set the selectedSLink_ pointer.
 */
void SApplication::addSelectedSLink( SLink *lk )
{
    // If this is not the first one in selection, we have no single 
    // currentSelectedSLink.
    if( selectionList_->count() ) {
        currentSelectedSLink_ = NULL;
    } else {
        currentSelectedSLink_ = lk;
    }
    selectionList_->append( lk );    
    QObject::connect( lk, SIGNAL( destroyed() ), 
                      this, SLOT( unselectSLink() ) );    
}

/**
 * Unselect the given SLink, removing it from the selection list.
 */
void SApplication::unselectSLink( SLink *lk )
{
    bool wasFound = selectionList_->removeOne( lk );
    if( wasFound ) {
        QObject::disconnect( 
            lk, SIGNAL( destroyed() ), 
            this, SLOT( unselectSLink() ) );        
        if( selectionList_->count()==1 ) {
            currentSelectedSLink_ = selectionList_->first();
        } else {
            currentSelectedSLink_ = NULL;
        }
    }
}

/**
 * Clears the selection list.
 */
void SApplication::clearSelection()
{
    currentSelectedSLink_ = NULL;
    while( selectionList_->count() ) {
        unselectSLink( selectionList_->first() );
    }
}

/**
 * Returns the currently selected slink or NULL, if many have been selected.
 */
SLink *SApplication::getCurrentSelectedSLink() const
{
    return currentSelectedSLink_;
}

/**
 * Clears the selection and adds one slink.
 */
void SApplication::setSelectedSLink( SLink *newSelection )
{
    clearSelection();
    addSelectedSLink( newSelection );
}

/**
 * Slot invoked, if any of the selected slinks become destroyed.
 */
void SApplication::unselectSLink()
{
    SLink *sendingLink = (SLink *) (const SLink *) sender();
    if( sendingLink ) {
        unselectSLink( sendingLink );
    } else {
        qWarning( "SApplication::unselectSLink(): Warning: "
                  "Sending object was unset.\n" );
    }
}

SApplication &SApplication::app()
{
    return *singleton_;
}

std::shared_ptr<twSpeaker> SApplication::getSpeaker() const
{
    return t3Speaker_;
}

tw303aEnvironment *SApplication::get303aEnvironment() const
{
    return t3Env_;
}

void SApplication::setGlobalLocatorPos( offset_t o )
{
    // UI-thread setter: store and repaint immediately. Safe to emit here.
    offset_t old = globalLocatorPos_.exchange( o );
    lastShownLocator_ = o;
    emit globalLocatorMoved( o, old );

    // If the engine is live, reposition PLAYBACK too — the playback position is
    // owned by the AudioEngine, not the component graph, so a click/transport
    // seek that only moved the locator would leave audio playing from the old
    // spot. requestSeek is RT-safe and a no-op when nothing is playing.
    if( isPlaying_ && t3Speaker_ )
        t3Speaker_->requestSeek( o );

    // A locate while running: the MIDI queue describes a playhead that no
    // longer exists, so it is dropped, every sounding note is released and the
    // chase is re-issued at the new position (D6). Told EXPLICITLY rather than
    // inferred from a jump in the atomic - a forward seek of less than a tick
    // is indistinguishable from ordinary advance.
    if( isPlaying_ && midiOutPump_ ) midiOutPump_->locate( o );

    // The live pump renders every block AT A POSITION, and while STOPPED that
    // position is a virtual counter from the LOCATOR (design D2). A seek moves
    // the anchor and the automation hold, so it is a plan rebuild plus one
    // explicit reposition - drift detection alone would make the first block
    // after the seek a race.
    if( liveMonitor_ ) liveMonitor_->seeked();
}

void SApplication::setGlobalLocatorPosRealtime( offset_t o )
{
    // Audio-thread setter: atomic stores ONLY. No emit (see header rationale).
    globalLocatorPos_.store( o, std::memory_order_relaxed );
    // The publication COUNTER, not just the value: a position that has not
    // moved is still evidence that the device asked for a block, which is what
    // the MIDI-out pump anchors its clock on (see locatorPublishSeq).
    locatorPublishSeq_.fetch_add( 1, std::memory_order_relaxed );
}

void SApplication::pumpLocator()
{
    // Main thread: reflect the audio/record thread's advance into the playhead.
    offset_t now = globalLocatorPos_.load( std::memory_order_relaxed );
    if( now != lastShownLocator_ ) {
        offset_t old = lastShownLocator_;
        lastShownLocator_ = now;
        emit globalLocatorMoved( now, old );
        // Advance under playback/recording only (this poll never runs for a
        // manual seek) — the view-follows-playhead feature keys off this.
        emit locatorAdvanced( now, old );
    }
    // Self-stop once nothing is driving the locator anymore (playback stopped and
    // recording finished). Recording ends asynchronously on the worker thread, so
    // this poll is where we notice it.
    if( locatorTimer_ && !isPlaying_ && !isRecordingActive() )
        locatorTimer_->stop();
}

// ------------------------------------------------ level metering (prop. 34) ---

bool SApplication::isMeteringActive() const
{
    return meterTimer_ && meterTimer_->isActive();
}

offset_t SApplication::meterLatencyFrames() const
{
    // Only the live output path has a device buffer sitting between the position
    // the RT thread published and the sound the ear gets. A render publishes
    // positions faster than realtime and is not metered at all (startMetering).
    if( !isPlaying_ || !t3Speaker_ ) return 0;

    audio::AudioBackend *backend = t3Speaker_->getBackend();
    if( !backend ) return 0;

    const std::uint32_t devLatency = backend->getLatencyFrames();
    if( devLatency == 0 ) return 0;   // backend does not know — do not guess

    // outputLatencyFrames is in DEVICE frames at the DEVICE rate, but the locator
    // counts PROJECT frames. Skipping this conversion mis-compensates by the rate
    // ratio — ~9% for a 44.1 kHz project on a 48 kHz device.
    const std::uint32_t devRate  = backend->getConfig().sampleRate;
    const int           projRate = t3Env_ ? t3Env_->getSRate() : 0;
    if( devRate == 0 || projRate <= 0 ) return (offset_t) devLatency;

    return (offset_t) ( ( (double) devLatency * (double) projRate )
                        / (double) devRate + 0.5 );
}

offset_t SApplication::outputBufferFramesProject() const
{
    if( !t3Speaker_ ) return 0;
    audio::AudioBackend *backend = t3Speaker_->getBackend();
    if( !backend ) return 0;
    const std::uint32_t devBuffer = backend->getConfig().bufferFrames;
    if( devBuffer == 0 ) return 0;
    const std::uint32_t devRate  = backend->getConfig().sampleRate;
    const int           projRate = t3Env_ ? t3Env_->getSRate() : 0;
    if( devRate == 0 || projRate <= 0 ) return (offset_t) devBuffer;
    return (offset_t) ( ( (double) devBuffer * (double) projRate )
                        / (double) devRate + 0.5 );
}

void SApplication::startMetering()
{
    // A render drives the locator through renderSession_->onPosition, faster than
    // realtime, so metering one would sweep the bars at whatever speed the render
    // happens to run. Recording IS metered: monitoring playback is live.
    if( isRenderingActive() || !meterTimer_ ) return;

    if( !meterClock_.isValid() ) meterClock_.start();
    meterTailTicks_ = METER_TAIL_TICKS;
    emit meterReset();
    if( !meterTimer_->isActive() ) meterTimer_->start();
}

bool SApplication::masterLevel( offset_t pos, twLevelSample &out )
{
    // Re-bind every call rather than on a project-change hook: setTap() no-ops
    // when the component is unchanged and resets the probe when it is not, so a
    // new project / rewired graph is picked up without another lifecycle edge to
    // keep in step.
    masterProbe_.setTap( rootComponent() );
    return masterProbe_.advanceTo( pos, out );
}

bool SApplication::masterLevel( offset_t pos, twLevelSampleSet &out, int wantLanes )
{
    masterProbe_.setTap( rootComponent() );
    return masterProbe_.advanceTo( pos, out, wantLanes );
}

int SApplication::masterChannels()
{
    // The mixer root's declared width IS the project's (SStdMixer takes it from
    // SProject::channels() since B4), so deriving it here cannot drift from what
    // the graph actually produces the way a copied number can — the same reason
    // RenderParams::channels = 0 means "ask the graph".
    std::shared_ptr<twComponent> root = rootComponent();
    const int n = root ? (int) root->getOutputChannels() : 1;
    return n > 0 ? n : 1;
}

void SApplication::pumpMeters()
{
    // A LIVE LANE keeps the meters alive too (design D9): monitoring an input
    // with the transport stopped is exactly the case where a decaying bar would
    // be a lie about a signal that is still there.
    const bool live = isPlaying_ || isRecordingActive()
                      || ( liveMonitor_ && liveMonitor_->active() );

    offset_t pos = globalLocatorPos_.load( std::memory_order_relaxed )
                   - meterLatencyFrames();
    if( pos < 0 ) pos = 0;

    emit meterTick( pos, meterClock_.elapsed(), live );

    // Keep ticking for a while after the transport stops so the bars can DECAY to
    // the floor. Stopping the moment playback ends would leave them frozen
    // mid-level, which reads as a rendering bug rather than as "stopped".
    if( live ) {
        meterTailTicks_ = METER_TAIL_TICKS;
    } else if( meterTailTicks_ > 0 ) {
        --meterTailTicks_;
    } else if( meterTimer_ ) {
        meterTimer_->stop();
    }
}

// ---------------------------------------------- plugin discovery (08 M2) ----

void SApplication::initPluginRegistry()
{
    audio::twPluginRegistry &reg = audio::pluginRegistry();
    SSettings               &s   = SSettings::instance();

    // The cache lives next to smaragd.ini, not in the sidecar store: the
    // sidecar store keys on a content hash of audio PCM and caps itself with an
    // LRU, which would silently evict the plugin table.
    reg.setCachePath(
        QDir( s.configDir() ).filePath( "plugincache.json" ).toStdString() );

    // The APP supplies the probe path, so the registry stays dumb and headlessly
    // testable — and on macOS the executable lives inside the .app bundle next
    // to the main binary, which only the app can know.
    const QString probe = QDir( applicationDirPath() ).filePath(
#ifdef Q_OS_WIN
        QStringLiteral( "smaragd_pluginprobe.exe" )
#else
        QStringLiteral( "smaragd_pluginprobe" )
#endif
    );
    if( QFileInfo::exists( probe ) ) {
        reg.setProbeExecutable( probe.toStdString() );
    } else {
        // Not fatal: the registry falls back to probing in-process. That is
        // still safe against a corrupt file, but a plugin that crashes while
        // being instantiated would then take the app with it.
        TW_LOGW( "plugins", "[app] no plugin probe executable at '%s'; plugin "
                 "scanning will run IN-PROCESS, without crash isolation",
                 probe.toUtf8().constData() );
    }

    pushPluginSearchPaths();

    pluginScanTimer_ = new QTimer( this );
    pluginScanTimer_->setInterval( 200 );
    connect( pluginScanTimer_, &QTimer::timeout, this, &SApplication::pumpPluginScan );

    if( s.pluginScanOnStartup() ) rescanPlugins( false );
}

void SApplication::pushPluginSearchPaths()
{
    std::vector<std::string> dirs;
    for( const QString &d : SSettings::instance().pluginSearchPaths() ) {
        const QString t = d.trimmed();
        if( !t.isEmpty() ) dirs.push_back( t.toStdString() );
    }
    audio::pluginRegistry().setSearchPaths( std::move( dirs ) );
}

void SApplication::rescanPlugins( bool force )
{
    // Re-read the list first: the options dialog may have just changed it.
    pushPluginSearchPaths();
    if( audio::pluginRegistry().rescanAsync( force ) && pluginScanTimer_ )
        pluginScanTimer_->start();
}

bool SApplication::isPluginScanActive() const
{
    return audio::pluginRegistry().isScanning();
}

void SApplication::pumpPluginScan()
{
    if( audio::pluginRegistry().isScanning() ) return;
    if( pluginScanTimer_ ) pluginScanTimer_->stop();
    // Emitted from the main thread, which is the whole point of this poll.
    emit pluginScanFinished();
}

QString SApplication::pluginScanStatusText() const
{
    audio::twPluginRegistry  &reg = audio::pluginRegistry();
    const audio::twPluginScanStats st = reg.scanStats();

    if( reg.isScanning() ) {
        return QString( "Scanning %1 module(s)…" ).arg( st.modulesFound );
    }
    if( st.modulesFound == 0 && st.pluginsFound == 0 ) {
        return QString( "%1 plugin(s); no plugin modules found in the "
                        "directories above." )
            .arg( (int) reg.plugins().size() );
    }
    QString txt = QString( "%1 plugin(s) from %2 module(s) — %3 probed, %4 cached" )
        .arg( st.pluginsFound ).arg( st.modulesFound )
        .arg( st.modulesProbed ).arg( st.modulesCached );
    if( st.modulesSkipped > 0 )
        txt += QString( ", %1 skipped (previously failed)" ).arg( st.modulesSkipped );
    if( st.modulesFailed > 0 )
        txt += QString( ", %1 failed" ).arg( st.modulesFailed );
    return txt;
}

offset_t SApplication::getGlobalLocatorPos() const
{
    return globalLocatorPos_.load( std::memory_order_relaxed );
}

SApplication::SApplication( int &argc, char **argv )
    : QApplication( argc, argv ),
      actionHistory_( NULL ),
      currentSelectedSLink_( NULL ),
      globalLocatorPos_( 0 ),
      isPlaying_( false ),
      renderSession_( nullptr ),
      currentProject_( NULL ),
      recordingSession_( nullptr )
{
    setOrganizationName( "Smaragd" );
    setApplicationName( "smaragd" );

    singleton_ = this;
    // ~30 Hz playhead poll: the audio thread stores its position lock-free; this
    // main-thread timer turns that into globalLocatorMoved repaints while playing
    // so the realtime thread never touches Qt. Started/stopped by setPlaying().
    locatorTimer_ = new QTimer( this );
    locatorTimer_->setInterval( 33 );
    connect( locatorTimer_, SIGNAL( timeout() ), this, SLOT( pumpLocator() ) );
    // Proposal 34: the metering pump. Same 33 ms as the playhead so meters and
    // playhead stay visually locked, but its OWN timer — see pumpMeters().
    meterTimer_ = new QTimer( this );
    meterTimer_->setInterval( 33 );
    connect( meterTimer_, &QTimer::timeout, this, &SApplication::pumpMeters );
    // Proposal 37 P7b: the MIDI-out pump. Built here, before any project
    // exists, because its enumeration probe port must be the FIRST MidiOutput
    // this process constructs (see SMidiOutPump's constructor). Its own timer
    // only runs between play and stop.
    midiOutPump_.reset( new SMidiOutPump( this ) );
    // The live lane (proposal 21 L1b). A sibling of the MIDI-out pump in every
    // respect that matters: constructed with the app, destroyed FIRST, and the
    // owner of a std::thread whose join must happen on the main thread.
    // The MIDI input ports (proposal 21 L2), BEFORE the live monitor that
    // acquires sinks from them and before any project can arm a track. Its own
    // enumeration probe must also be constructed before any listening port, so
    // that a headless case's `midi-in-event` reaches the port the live lane
    // drains rather than the probe (see SMidiInputHub's constructor).
    midiInputHub_.reset( new SMidiInputHub() );
    liveMonitor_.reset( new SLiveMonitor( this ) );
    automationRecorder_.reset( new SAutomationRecorder( this ) );
    selectionList_ = new SSelectionList();
    t3Env_ = new tw303aEnvironment;
    t3Env_->setBufferSize( 4096 );
    SAppContext::setInstance( this );        // core modules reach us only via this
    t3Speaker_ = std::make_shared<twSpeaker>( *t3Env_ );
    t3Speaker_->setPlaybackContext( this );   // app services; we outlive the speaker
    t3Speaker_->init();
    actionHistory_ = new SActionHistory( this );

    // Restore the audio output device chosen in a previous session.
    QString devId = SSettings::instance().audioDeviceId();
    if( !devId.isEmpty() ) t3Speaker_->setOutputDevice( devId.toStdString() );

    // Proposal 27: app-global derived-data sidecar store (content-addressed,
    // safe to delete wholesale). SMARAGD_SIDECAR_DIR=off disables it,
    // =<path> relocates it; default is the per-user cache location.
    QByteArray sidecarDir = qgetenv( "SMARAGD_SIDECAR_DIR" );
    if( sidecarDir != "off" ) {
        QString root = sidecarDir.isEmpty()
            ? QStandardPaths::writableLocation( QStandardPaths::CacheLocation )
                  + "/sidecars"
            : QString::fromLocal8Bit( sidecarDir );
        twSidecarStore::instance().setRoot( root.toStdString() );
    }

    // Proposal 08 M2: plugin discovery. Last, because it may start a background
    // scan and everything above it must already be in place.
    initPluginRegistry();
}

SApplication::~SApplication()
{
    // Stop and join the scan worker BEFORE anything else goes away: it holds a
    // QProcess and writes the cache, and the registry outlives us (it is a
    // static). stopScan(), not waitForScan(): a scan still walking this
    // machine's installed modules must not hold the process open for minutes,
    // and it must not still be alive (and logging) once static destruction
    // starts -- see plan/STATE.md 2026-08-16.
    audio::pluginRegistry().stopScan();
    // The MIDI scheduler threads join HERE, on the main thread, while the log
    // sink is still alive - not during static destruction, which is where this
    // repo has already recorded a teardown hang of exactly that shape.
    // The live lane goes first: it stops and JOINS the pump thread and closes
    // the input device, and both of those must happen before the speaker it
    // hands audio to is destroyed.
    liveMonitor_.reset();
    // AFTER the live monitor: it holds fan-out sinks and a thru route into a
    // scheduler, and both belong to ports this owns.
    midiInputHub_.reset();
    midiOutPump_.reset();
    automationRecorder_.reset();
    DTOR_DEL( actionHistory_ );
    t3Speaker_.reset();
    DTOR_DEL( t3Env_ );
    DTOR_DEL( selectionList_ );
}

SActionHistory *SApplication::actionHistory() const
{
    return actionHistory_;
}

void SApplication::submitAction(SAction *action)
{
    if (actionHistory_) {
        actionHistory_->submit(action);
    }
}

// audio::PlaybackContext: root of the component graph to play (null when no
// project is open). Shared by the speaker, the render path, and monitoring.
std::shared_ptr<twComponent> SApplication::rootComponent()
{
    if (!currentProject_) return nullptr;
    SObject *root = currentProject_->getRootComponent();
    return root ? root->getRootComponent() : nullptr;
}

audio::RenderSession *SApplication::renderSession() const
{
    return renderSession_.get();
}

bool SApplication::isRenderingActive() const
{
    return renderSession_ && renderSession_->isRunning();
}

// Proposal 37 D4 / 4.4 - see the header for WHY and for the call-site rules.
void SApplication::beginRun( offset_t pos )
{
    if( !currentProject_ ) return;
    SObject *root = currentProject_->getRootComponent();
    if( !root ) return;

    std::vector<STrack *> tracks;
    sinstruments::collectInstrumentTracks( root, tracks );
    if( tracks.empty() ) return;      // no instrument => no barrier, no cost

    int n = 0;
    for( STrack *tr : tracks ) {
        SPluginSlot *slot = tr->instrumentSlot();
        if( !slot ) continue;
        // ORDER MATTERS (header): forget first, bump second.
        slot->forgetContinuity();
        tr->invalidateRenderPathRange( pos,
                                       std::numeric_limits<offset_t>::max() );
        ++n;
    }
    TW_LOGI( "app", "run barrier at %lld: %d instrument track(s) reset and "
                    "invalidated to the end", (long long) pos, n );
}

void SApplication::startRender(const audio::RenderParams &params)
{
    // Always recreate for reproducibility
    renderSession_ = std::make_unique<audio::RenderSession>();

    if (isPlaying_) {
        t3Speaker_->stopOutput();
        isPlaying_ = false;
    }

    // A RENDER SUSPENDS EVERY LIVE LANE for its duration (proposal 21 D4).
    // Export ignores the split, as in Cubase; the practical requirement is
    // harder than the aesthetic one: `beginRun` below walks instrument tracks
    // and the render's own demands freeze the whole graph, so a live-owned
    // processor would answer silence to both. Suspending BEFORE beginRun is
    // what makes "the barrier never meets a live-owned track" true by order
    // rather than by luck. It comes back afterwards as a FRESH arm.
    if (liveMonitor_) liveMonitor_->suspendForRender();

    // Get the synth output component
    std::shared_ptr<twComponent> synthOutput = rootComponent();
    if (!synthOutput) {
        // TODO: Emit error signal to UI
        return;
    }

    // THE RUN BARRIER (proposal 37 D4), on the MAIN thread and BEFORE the
    // render worker exists - so it is ordered ahead of the run's first demand
    // by construction, not by a race we would have to reason about. This is the
    // determinism hole F4: without it, a render started after in-process
    // playback continues the playback run's synth voices and produces different
    // bytes from the same render in a fresh process.
    // The position is the render RANGE start, computed exactly as
    // RenderSession does (llround of the seconds), so the barrier covers the
    // first page the render will ask for.
    {
        const int rate = t3Env_ ? t3Env_->getSRate() : 48000;
        beginRun( (offset_t) std::llround( params.startTimeSec * (double) rate ) );
    }

    // Playhead tracking: the render thread publishes positions through this
    // callback (realtime-safe atomic store; the engine has no app knowledge).
    renderSession_->onPosition = [this](std::uint64_t pos) {
        setGlobalLocatorPosRealtime((offset_t) pos);
    };

    // Proposal 19 dataflow stage 4: the render is a scheduler CONSUMER — it
    // demands watermarks and waits at the edge; the shared worker pool
    // executes the page DAG (bound inputs, serial cursors, dedup). The old
    // render-quiesce pause() must NOT run on this path: pausing the
    // revalidator would gate the very workers the render's demands execute
    // on. Coordination IS the scheduler now (the Inv-3 retirement the
    // proposal anticipated). The pause remains only for the legacy pull path
    // (no revalidator available, e.g. SMARAGD_REVAL_WORKERS=0).
    SProject *proj = currentProject_;
    CaptureRevalidator *sched = proj ? proj->getRevalidator() : nullptr;
    if (sched) {
        renderSession_->setScheduler(sched);
        // Background aspect jobs (preview recomputation etc.) mutate shared
        // component state and made the render NONDETERMINISTIC when they
        // overlapped it (goldens diverged run-to-run). Quiesce THEM for the
        // render's lifetime while the render's own graph demands keep
        // executing on the pool — the background-only variant of the old
        // full pause ("offline renders stay exact").
        sched->pauseBackground();
        renderSession_->onComplete =
            [this, sched](bool /*success*/, const char * /*error*/) {
                sched->resumeBackground();
                // The session thread is Qt-free by contract, so the live lane
                // is re-armed by a QUEUED call that runs on the main thread.
                QMetaObject::invokeMethod( this, "resumeLiveAfterRender",
                                           Qt::QueuedConnection );
            };
    } else if (proj) {
        proj->pauseRevalidation();
        renderSession_->onComplete =
            [this, proj](bool /*success*/, const char * /*error*/) {
                proj->resumeRevalidation();
                QMetaObject::invokeMethod( this, "resumeLiveAfterRender",
                                           Qt::QueuedConnection );
            };
    }

    // Start rendering
    int sampleRate = t3Env_ ? t3Env_->getSRate() : 48000;
    bool started = renderSession_->start(synthOutput, params,
                                         static_cast<std::uint32_t>(sampleRate));
    if (!started) {
        // Render never launched → onComplete will not fire; resume now.
        if (sched) sched->resumeBackground();
        else if (proj) proj->resumeRevalidation();
        resumeLiveAfterRender();
    }
}

void SApplication::resumeLiveAfterRender()
{
    if( liveMonitor_ ) liveMonitor_->resumeAfterRender();
}

void SApplication::keyboardNoteOn( int key, int velocity, int channel )
{
    if( audio::KeyboardMidiInput *kb = audio::KeyboardMidiInput::active() )
        kb->noteOn( key, velocity, channel );
}

void SApplication::keyboardNoteOff( int key, int channel )
{
    if( audio::KeyboardMidiInput *kb = audio::KeyboardMidiInput::active() )
        kb->noteOff( key, channel );
}

void SApplication::liveLanesChanged()
{
    if( liveMonitor_ ) liveMonitor_->refresh();
}

void SApplication::setPlaybackRunning( bool play )
{
    // BEFORE the frozen lane moves (see SLiveMonitor::transportAboutToChange):
    // startOutput() starts the readahead immediately, and a track that monitor
    // Auto is about to release must stop being live-owned first.
    if( liveMonitor_ ) liveMonitor_->transportAboutToChange( play );
    if( !t3Speaker_ ) return;
    if( play ) {
        // Run barrier immediately before startOutput(), which performs the
        // engine's pre-readahead seekTo(locator) + startReadahead() on THIS
        // thread - so the barrier precedes the readahead's first demand (D4).
        beginRun( getGlobalLocatorPos() );
        t3Speaker_->startOutput();
        setPlaying( true );
    } else {
        t3Speaker_->stopOutput();
        setPlaying( false );
    }
}

audio::RecordingSession *SApplication::recordingSession() const
{
    return recordingSession_.get();
}

bool SApplication::isRecordingActive() const
{
    return recordingSession_ && recordingSession_->isRunning();
}

void SApplication::startRecording(const audio::RecordingParams &params)
{
    if (!recordingSession_) {
        recordingSession_ = std::make_unique<audio::RecordingSession>();
    }

    // Remember where capture begins so the view can draw the growing in-progress
    // region (the worker advances the locator from here as it captures).
    recordingStartFrame_ = getGlobalLocatorPos();

    // The engine session has no app knowledge: hand it the start position and
    // a realtime-safe playhead callback (atomic store only — record thread!).
    audio::RecordingParams p = params;
    p.startLocatorFrames = (std::uint64_t) recordingStartFrame_;
    recordingSession_->onPosition = [this](std::uint64_t pos) {
        setGlobalLocatorPosRealtime((offset_t) pos);
    };

    // Start capture first, so isRecordingActive() is already true before the
    // monitoring playback below produces its first buffer. That keeps the record
    // worker the sole locator authority (the playback callback won't advance the
    // locator while recording — see twSpeaker).
    recordingSession_->start(p);

    // Monitoring: play the existing arrangement so the user hears it while
    // recording. Output is best-effort — capture and the playhead still work if
    // it fails (the worker drives the locator regardless).
    if (!isPlaying_ && currentProject_) {
        // The root is a PRESENCE check only: there is nothing to monitor
        // without one.
        if (currentProject_->getRootComponent()) {
            // NO graph seek here (same reason as SMainWindow::startPlaying):
            // an external seek cascade races in-flight page freezes, which
            // serialize on cursorMutex_ while a seek takes only mutex(). The
            // monitoring playback starts at the locator because the engine
            // pulls pages BY POSITION, not because the graph's cursors were
            // moved.
            //
            // The monitoring playback is a RUN like any other (D4): it is the
            // readahead reading the arrangement, instruments included, so it
            // gets the same barrier immediately before startOutput().
            beginRun( getGlobalLocatorPos() );
            t3Speaker_->startOutput();
            isPlaying_ = true;
        }
    }

    // Drive the playhead repaints while recording (the worker stores positions
    // lock-free; pumpLocator turns them into repaints and self-stops at the end).
    if( locatorTimer_ && !locatorTimer_->isActive() )
        locatorTimer_->start();

    // Meters follow the monitoring playback started above. isPlaying_ was set
    // directly here rather than through setPlaying(), so arm the pump explicitly.
    startMetering();
}

void SApplication::setSelectionFromPaths(const QList<QList<int>> &paths)
{
    clearSelection();
    addSelectionFromPaths(paths);
}

void SApplication::addSelectionFromPaths(const QList<QList<int>> &paths)
{
    SSelectionManager mgr;
    SSelectionList addedLinks = mgr.pathsToLinks(paths, currentProject_);
    for (SLink *link : addedLinks) {
        if (link && !isSLinkSelected(link)) {
            addSelectedSLink(link);
        }
    }
}

void SApplication::removeSelectionFromPaths(const QList<QList<int>> &paths)
{
    SSelectionManager mgr;
    SSelectionList removedLinks = mgr.pathsToLinks(paths, currentProject_);
    for (SLink *link : removedLinks) {
        if (link && isSLinkSelected(link)) {
            unselectSLink(link);
        }
    }
}

void SApplication::toggleSelectionFromPaths(const QList<QList<int>> &paths)
{
    SSelectionManager mgr;
    SSelectionList toggleLinks = mgr.pathsToLinks(paths, currentProject_);
    for (SLink *link : toggleLinks) {
        if (link) {
            if (isSLinkSelected(link)) {
                unselectSLink(link);
            } else {
                addSelectedSLink(link);
            }
        }
    }
}

QList<QList<int>> SApplication::getCurrentSelectionPaths() const
{
    SSelectionManager mgr;
    return mgr.linksToPaths(*selectionList_, currentProject_);
}

void SApplication::submitSetSelectionAction(SLink *link)
{
    if (!link) return;
    SSelectionManager mgr;
    QList<QList<int>> paths = mgr.linksToPaths({link}, currentProject_);
    SAction *action = new SSetSelectionAction(paths);
    submitAction(action);
}

void SApplication::submitAddSelectionAction(SLink *link)
{
    if (!link) return;
    SSelectionManager mgr;
    QList<QList<int>> paths = mgr.linksToPaths({link}, currentProject_);
    SAction *action = new SAddToSelectionAction(paths);
    submitAction(action);
}

void SApplication::submitToggleSelectionAction(SLink *link)
{
    if (!link) return;
    SSelectionManager mgr;
    QList<QList<int>> paths = mgr.linksToPaths({link}, currentProject_);
    SAction *action = new SToggleSelectionAction(paths);
    submitAction(action);
}

void SApplication::submitClearSelectionAction()
{
    SAction *action = new SClearSelectionAction();
    submitAction(action);
}

void SApplication::setTestOutputDir(const QString &path)
{
    testOutputDir_ = path;
}

QString SApplication::testOutputDir() const
{
    return testOutputDir_;
}

bool SApplication::ensureOutputDirExists() const
{
    if (testOutputDir_.isEmpty()) {
        return false;
    }

    QDir dir(testOutputDir_);
    if (dir.exists()) {
        return true;
    }

    return dir.mkpath(".");
}
