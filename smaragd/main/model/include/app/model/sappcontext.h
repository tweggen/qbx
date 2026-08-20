#ifndef _SAPPCONTEXT_H_
#define _SAPPCONTEXT_H_

#include <QList>
#include <QString>

#include "tw/core/twtypes.h"

class SProject;
class SLink;
class tw303aEnvironment;

namespace audio {
struct RenderParams;
}

/**
 * The narrow application context the core modules (model, actions,
 * persistence, selection, object slices) are allowed to see — proposal 14,
 * Phase 6. SApplication implements this; nothing below the UI layer may
 * include app/shell/sapplication.h anymore.
 *
 * Method names deliberately match SApplication's so the implementation is
 * pure inheritance; keep new methods to the MINIMUM a non-UI module truly
 * needs — every addition here is a coupling everyone below the shell gets.
 *
 * Threading: all methods are UI-thread unless noted; none may be called
 * from audio/render/record worker threads.
 */
class SAppContext {
public:
    virtual ~SAppContext() = default;

    // Document / engine context
    virtual SProject *getCurrentProject() const = 0;
    virtual tw303aEnvironment *get303aEnvironment() const = 0;

    // Re-fetch the project root's output and connect it to the speaker
    // (call after structural graph changes: add/remove/reparent track).
    virtual void rewireSpeaker() = 0;

    // Selection state (lives with the app until it grows its own service)
    virtual bool isSLinkSelected( SLink * ) const = 0;
    virtual void setSelectionFromPaths( const QList<QList<int>> &paths ) = 0;

    // Root-carrying variants (proposal 09 D21 / §3). A selection ACTION knows
    // which root its paths address and must write into THAT root's list --
    // otherwise a scripted set-selection over "Drums:0,0" lands in whichever
    // tab happens to be active. Non-pure and defaulting to the active-root
    // forms, so an implementation with no notion of roots (the test stub) is
    // unaffected.
    virtual void setSelectionFromPathsFor( const QList<QList<int>> &paths,
                                           const QString &root )
    { (void) root; setSelectionFromPaths( paths ); }
    virtual QList<QList<int>> getCurrentSelectionPathsFor( const QString &root ) const
    { (void) root; return getCurrentSelectionPaths(); }
    virtual void addSelectionFromPaths( const QList<QList<int>> &paths ) = 0;
    virtual void removeSelectionFromPaths( const QList<QList<int>> &paths ) = 0;
    virtual void toggleSelectionFromPaths( const QList<QList<int>> &paths ) = 0;
    virtual QList<QList<int>> getCurrentSelectionPaths() const = 0;

    // Headless test artifacts
    virtual QString testOutputDir() const = 0;
    virtual bool ensureOutputDirExists() const = 0;

    // Services (for generic actions)
    virtual void startRender( const audio::RenderParams &params ) = 0;
    virtual bool isRenderingActive() const = 0;
    // Start/stop transport playback (speaker output + playing flag).
    virtual void setPlaybackRunning( bool play ) = 0;
    // The global locator, in project frames. Needed by proposal 37 P5: a
    // set-track-volume / set-track-mute on a track that carries a READ lane
    // becomes a point ON that lane, and "where" is the locator.
    virtual offset_t getGlobalLocatorPos() const = 0;

    // A LIVE-LANE input changed (proposal 21 L1b, design §3 "plan rebuild
    // triggers"): a track was armed or disarmed, or its trackInput /
    // monitorMode moved. The shell rebuilds the live plan; every other
    // implementation may ignore it, which is why it is not pure.
    //
    // It is a NOTIFICATION, not a command: the verbs stay pure model edits and
    // the arm/disarm sequence (retire -> setLiveOwned -> rewire -> epoch ->
    // publish) lives in one place, where the pump and the speaker are.
    virtual void liveLanesChanged() {}

    // Process-wide instance, set once by SApplication at startup.
    static void setInstance( SAppContext *ctx );
    static SAppContext &get();

private:
    static SAppContext *instance_;
};

#endif
