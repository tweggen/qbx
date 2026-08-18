// app/media/smediadrop.h — what a `media:` drop MEANS.
//
// Proposal 38 §B.5/§B.6, gate 3. The arranger's new branch is five lines and
// this is everything under them:
//
//     } else if( payload.startsWith( QStringLiteral( "media:" ) ) ) {
//         const SMediaRef ref = SMediaRef::fromUri( payload.mid( 6 ) );
//         smediadrop::placeWhenLocal( ref, track, timePos );
//     }
//
// PLACEMENT IS ALWAYS THE EXISTING `add-sample` OVER A LOCAL PATH. There is no
// new action type, no placeholder clip and no SCut work anywhere in this file:
// the media layer's job ENDS at producing that path. Everything else follows
// from that sentence.
//
// Three things that are not obvious and that a "simplification" would break:
//
//   * THE PENDING PLACEMENT HOLDS THE TARGET'S IDENTITY, NEVER ITS INDEX-PATH
//     (§B.5, trap T18). An index-path is a position in a tree the user is free
//     to edit while a 40 MB file downloads: delete a track above the target, or
//     reorder a folder, and a path captured at drop time now names A DIFFERENT
//     TRACK — a clip landing silently on the wrong lane, which is worse than
//     not landing at all. It is held as a QPointer, which is that identity plus
//     the one thing a bare pointer or a serialized `id` cannot offer: it cannot
//     be confused by an address the allocator handed to a new track. A vanished
//     track places NOTHING and says so.
//   * THE PLACEMENT ITSELF IS A HOOK, and that is a LAYER fact rather than a
//     taste. This module is app_core; SAddSampleAction is app_objects, one
//     layer UP, so nothing here can construct one — the include does not
//     resolve and no edit to tools/check_layering.py would make it. The shell
//     installs the hook once, exactly as it injects the plugin scan cache's
//     path (SApplication::initPluginRegistry). A unit test installs its own.
//   * UNDO OF A DEFERRED DROP REMOVES THE CLIP AND LEAVES THE COPIED FILE
//     BEHIND. The project copy is not part of SAddSampleAction, so its undo
//     cannot know about it. That asymmetry is ACCEPTED and documented (§B.6 and
//     main/media/CONTRACT.md); the resources dock's Cleanup… dialog is the
//     precedent for this class of orphan. Do not "fix" it by hooking the
//     action.
//
// NO WIDGET, EVER (CONTRACT.md) — which is why the status message is a hook
// too, rather than a QWidget* the design sketch passed in.

#ifndef SMEDIADROP_H
#define SMEDIADROP_H

#include "app/media/smediaref.h"

#include "tw/core/twtypes.h"

#include <QString>

#include <functional>

class SObject;
class SProject;

namespace smediadrop {

// Submit the placement. Returns false when the target is no longer placeable
// (a deleted track, a dangling path), which is what turns into "placed
// nothing, and said so".
using PlaceSampleFn =
    std::function<bool( SObject *track, const QString &localPath,
                        offset_t timePos )>;
// One line for the user. The shell routes it to the status bar; a unit test
// collects it.
using StatusFn = std::function<void( const QString &message )>;

void setPlacementHook( PlaceSampleFn fn );
void setStatusHook( StatusFn fn );

// THE drop entry point. Places at once when the content is already local
// (exactly one undo step, synchronously); otherwise starts a fetch and places
// when it lands, at the frame and the TRACK the drop named.
void placeWhenLocal( const SMediaRef &ref, SObject *track, offset_t timePos );

// The current project was closed or SWITCHED. Every pending placement made
// against the outgoing project is dropped and its fetch cancelled (§B.5: a
// close is one case of a change, not the only one).
void projectChanged();

// Bookkeeping. `media-drop-wait` waits on pendingCount(); the rest exist so a
// gate can say "placed nothing" as a positive assertion rather than as the
// absence of a clip somewhere.
int  pendingCount();
int  placedCount();
int  failedCount();
int  abandonedCount();     // target track or project gone at completion
void resetCounters();

// --- the project copy (§B.6), exposed for media_cache_test ---------------

// A remote name sanitised for THIS file system. WebDAV permits ':', '?', '*',
// '|', a trailing dot and a trailing space; Windows permits none of them, and
// this repo is tested on Windows first. Never empty.
QString sanitiseFileName( const QString &name );

// Copy `localPath` to <projectDir>/media/<sanitised name>, disambiguating a
// collision as "name (2).ext" — NEVER overwriting — and reusing an existing
// copy whose bytes are already the same file. Returns the path the clip should
// reference; falls back to `localPath` itself (with a warning) when there is no
// project directory to copy into.
QString materialiseIntoProject( const QString &localPath,
                                const QString &displayName,
                                SProject *project );

}   // namespace smediadrop

#endif // SMEDIADROP_H
