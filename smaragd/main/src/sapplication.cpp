

#include <QTimer>
#include <QDir>

#include <tw303aenv.h>
#include <twspeaker.h>
#include <twwhitenoise.h>
#include <twconstant.h>

#include "sobject.h"
#include "slink.h"
#include "sapplication.h"
#include "sproject.h"
#include "ssettings.h"
#include "sactionhistory.h"
#include "saction.h"
#include "sselectionmanager.h"
#include "actions/ssetselectionaction.h"
#include "actions/saddtoselectionaction.h"
#include "actions/sremovefromselectionaction.h"
#include "actions/sclearselectionaction.h"
#include "actions/stoggleselectionaction.h"

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
    rewireSpeaker();
}

void SApplication::rewireSpeaker()
{
    // Drop any prior wiring first.
    getSpeaker()->setInput( 0, NULL );
    getSpeaker()->setInput( 1, NULL );
    if( !currentProject_ || !currentProject_->getRootComponent() ) return;

    // twRewire (the rewire root inside SStdMixer) returns NULL from
    // linkOutput() when nothing has been wired into it yet, so this call
    // is only meaningful once the graph has at least one track / bus.
    twComponent &root = currentProject_->getRootComponent()->getRootComponent();
    getSpeaker()->setInput( 0, root.linkOutput( 0 ) );
    if( root.getNOutputs() > 1 )
        getSpeaker()->setInput( 1, root.linkOutput( 1 ) );
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

twSpeaker *SApplication::getSpeaker() const
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
}

void SApplication::setGlobalLocatorPosRealtime( offset_t o )
{
    // Audio-thread setter: atomic store ONLY. No emit (see header rationale).
    globalLocatorPos_.store( o, std::memory_order_relaxed );
}

void SApplication::pumpLocator()
{
    // Main thread: reflect the audio/record thread's advance into the playhead.
    offset_t now = globalLocatorPos_.load( std::memory_order_relaxed );
    if( now != lastShownLocator_ ) {
        offset_t old = lastShownLocator_;
        lastShownLocator_ = now;
        emit globalLocatorMoved( now, old );
    }
    // Self-stop once nothing is driving the locator anymore (playback stopped and
    // recording finished). Recording ends asynchronously on the worker thread, so
    // this poll is where we notice it.
    if( locatorTimer_ && !isPlaying_ && !isRecordingActive() )
        locatorTimer_->stop();
}

void SApplication::setSpeakerMaxVal( sample_t /*maxVal*/ )
{
    // FIXME: insert for VU.
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
      currentProject_( NULL ),
      renderSession_( nullptr ),
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
    selectionList_ = new SSelectionList();
    t3Env_ = new tw303aEnvironment;
    t3Env_->setBufferSize( 4096 );
    t3Speaker_ = new twSpeaker( *t3Env_ );
    t3Speaker_->setPlaybackContext( this );   // app services; we outlive the speaker
    t3Speaker_->init();
    actionHistory_ = new SActionHistory( this );

    // Restore the audio output device chosen in a previous session.
    QString devId = SSettings::instance().audioDeviceId();
    if( !devId.isEmpty() ) t3Speaker_->setOutputDevice( devId.toStdString() );
}

SApplication::~SApplication()
{
    DTOR_DEL( actionHistory_ );
    DTOR_DEL( t3Speaker_ );
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
twComponent *SApplication::rootComponent()
{
    if (!currentProject_) return nullptr;
    SObject *root = currentProject_->getRootComponent();
    return root ? &root->getRootComponent() : nullptr;
}

audio::RenderSession *SApplication::renderSession() const
{
    return renderSession_.get();
}

bool SApplication::isRenderingActive() const
{
    return renderSession_ && renderSession_->isRunning();
}

void SApplication::startRender(const audio::RenderParams &params)
{
    // Always recreate for reproducibility
    renderSession_ = std::make_unique<audio::RenderSession>();

    if (isPlaying_) {
        t3Speaker_->stopOutput();
        isPlaying_ = false;
    }

    // Get the synth output component
    twComponent *synthOutput = rootComponent();
    if (!synthOutput) {
        // TODO: Emit error signal to UI
        return;
    }

    // Playhead tracking: the render thread publishes positions through this
    // callback (realtime-safe atomic store; the engine has no app knowledge).
    renderSession_->onPosition = [this](std::uint64_t pos) {
        setGlobalLocatorPosRealtime((offset_t) pos);
    };

    // Start rendering
    int sampleRate = t3Env_ ? t3Env_->getSRate() : 48000;
    renderSession_->start(synthOutput, params, static_cast<std::uint32_t>(sampleRate));
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
        if (SObject *root = currentProject_->getRootComponent()) {
            root->seekTo(getGlobalLocatorPos());
            t3Speaker_->startOutput();
            isPlaying_ = true;
        }
    }

    // Drive the playhead repaints while recording (the worker stores positions
    // lock-free; pumpLocator turns them into repaints and self-stops at the end).
    if( locatorTimer_ && !locatorTimer_->isActive() )
        locatorTimer_->start();
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
