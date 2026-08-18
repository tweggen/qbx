// app/media/smediatypes.h — the media-type categories and, in ONE place, the
// suffixes each one means.
//
// Proposal 38 §B.3 and §G.2. `kAudio` is BYTE-FOR-BYTE the seven suffixes of
// the Insert-sample dialog's "Audio files" entry
// (main/timeline/src/sstdmixerview.cpp) and that identity is the point of the
// list existing: the browser must never offer a file the importer cannot
// decode, and the dialog must never offer one the browser hides.
//
// ADDING A SUFFIX HERE IS A USER-VISIBLE CHANGE TO INSERT-SAMPLE and needs its
// own decision and its own line in a PR body. `.oga` is a legitimate candidate
// (libsndfile reads it) and is deliberately OUT — it was never requested and
// `ogg` already covers Ogg. `.m4a` / AAC is out for a harder reason: the
// import path (libsndfile + mpg123) cannot decode it, so listing it would
// offer the user a file that fails at the drop.
//
// `Midi` exists in the enum and in suffixesFor() so the filter control is
// multi-category BY SHAPE — the requester asked for a checkbox list — but the
// MVP registers only Audio.

#ifndef SMEDIATYPES_H
#define SMEDIATYPES_H

#include <QString>
#include <QStringList>

namespace smedia {

enum Category {
    Audio = 1,
    Midi  = 2,
};

// == sstdmixerview.cpp's "Audio files (*.wav *.mp3 *.flac *.aiff *.aif *.ogg
// *.opus)". Seven, in that order.
inline const QStringList kAudio = { QStringLiteral( "wav" ),
                                    QStringLiteral( "mp3" ),
                                    QStringLiteral( "flac" ),
                                    QStringLiteral( "aiff" ),
                                    QStringLiteral( "aif" ),
                                    QStringLiteral( "ogg" ),
                                    QStringLiteral( "opus" ) };

// Declared, not shipped: no source registers the Midi category in the MVP.
inline const QStringList kMidi = { QStringLiteral( "mid" ),
                                   QStringLiteral( "midi" ) };

// Every suffix the mask covers, in category order, de-duplicated. An empty
// mask means "no filter" and yields an EMPTY list — which every provider
// reads as "accept everything", never as "accept nothing".
const QStringList &suffixesFor( int categoryMask );

// The mask's canonical spelling for a describe() line or a settings key:
// "audio", "midi", "audio,midi", or "" for none.
QString categoryMaskToString( int categoryMask );
int      categoryMaskFromString( const QString &spelling );

// True when `fileName`'s suffix is in `suffixes` (case-insensitively). An
// EMPTY suffix list accepts everything — one place, so no provider has to
// re-decide what "no filter" means.
bool suffixAccepted( const QString &fileName, const QStringList &suffixes );

} // namespace smedia

#endif // SMEDIATYPES_H
