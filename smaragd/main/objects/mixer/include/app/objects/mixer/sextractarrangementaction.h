#ifndef SEXTRACTARRANGEMENTACTION_H
#define SEXTRACTARRANGEMENTACTION_H

#include "app/actions/saction.h"
#include "tw/graph/tw303aenv.h"
#include <QString>

// Action: move a set of tracks OUT of the master into a new named arrangement
// of their own, and put one asset clip where they were (proposal 09 D1/D6/D7,
// and D20 for the snippets).
//
// This is deliberately NOT a parameter of create-asset. create-asset is
// NON-DESTRUCTIVE: it windows a container that stays where it is, and the
// placement is a SECOND audible copy. This verb is destructive by design — the
// tracks LEAVE the master, so the arrangement is heard exactly once, through
// its placement (D8). Seven committed asset_* cases depend on the old meaning,
// and a user who wants "reuse this section elsewhere too" still wants it.
//
//   <extract-arrangement trackPaths="0;2,1" rangeStart="0" rangeEnd="192000"
//                        name="Drums" window="range" placeAt="0"/>
//
// trackPaths is SEMICOLON-separated because a single index path is already
// comma-separated. Paths are master-rooted. window is "range" (the asset
// windows [rangeStart,rangeEnd)) or "extent" (it windows the whole arrangement,
// and the range is then only advisory).
//
// WHAT HAPPENS TO MATERIAL OUTSIDE THE WINDOW (D20): it becomes SNIPPETS —
// one further asset per contiguous span of real material outside the window,
// registered by name and NOT placed anywhere. Nothing is lost and nothing is
// put on the timeline for the user; the snippets sit in the resource list as
// patches they may or may not use. The count is CAPPED and the cap is
// announced, never silent.
//
// Inverse: SDissolveArrangementAction, carrying a restore plan so the tracks go
// back to the exact parents and indices they came from.
class SExtractArrangementAction : public SAction {
public:
    SExtractArrangementAction() = default;
    SExtractArrangementAction( const QString &trackPaths,
                               offset_t rangeStart, offset_t rangeEnd,
                               const QString &name = QString(),
                               const QString &window = QStringLiteral("range"),
                               offset_t placeAt = -1 );

    QString name() const override { return QStringLiteral("extract-arrangement"); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    QStringList knownAttributes() const override
    { return { QStringLiteral("trackPaths"), QStringLiteral("rangeStart"),
               QStringLiteral("rangeEnd"),   QStringLiteral("name"),
               QStringLiteral("window"),     QStringLiteral("placeAt"),
               QStringLiteral("snippets") }; }

    // At most this many snippet assets. A dense arrangement must not silently
    // register ninety of them; past the cap the extraction logs what it dropped.
    static const int kMaxSnippets = 8;

private:
    QString  trackPaths_;
    offset_t rangeStart_ = 0;
    offset_t rangeEnd_   = 0;
    QString  arrName_;                              // empty => generate
    QString  window_ = QStringLiteral("range");
    offset_t placeAt_ = -1;                         // -1 => rangeStart_
    bool     snippets_ = true;
};

#endif // SEXTRACTARRANGEMENTACTION_H
