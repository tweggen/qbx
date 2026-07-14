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
    virtual void addSelectionFromPaths( const QList<QList<int>> &paths ) = 0;
    virtual void removeSelectionFromPaths( const QList<QList<int>> &paths ) = 0;
    virtual void toggleSelectionFromPaths( const QList<QList<int>> &paths ) = 0;
    virtual QList<QList<int>> getCurrentSelectionPaths() const = 0;

    // Headless test artifacts
    virtual QString testOutputDir() const = 0;
    virtual bool ensureOutputDirExists() const = 0;

    // True when running a headless/scripted test (`--test-case`): code below the
    // UI must suppress modal dialogs and other user prompts so tests never block
    // waiting for interaction. Log to stderr instead.
    virtual bool isNonInteractive() const = 0;

    // Services (for generic actions)
    virtual void startRender( const audio::RenderParams &params ) = 0;
    virtual bool isRenderingActive() const = 0;
    // Start/stop transport playback (speaker output + playing flag).
    virtual void setPlaybackRunning( bool play ) = 0;

    // Process-wide instance, set once by SApplication at startup.
    static void setInstance( SAppContext *ctx );
    static SAppContext &get();

private:
    static SAppContext *instance_;
};

#endif
