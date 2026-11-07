#ifndef _SSAMPLERLIBRARYACTIONS_H_
#define _SSAMPLERLIBRARYACTIONS_H_

// ssamplerlibraryactions.h — a LIBRARY program on a NassauDigital instance
// (nassau-digital DESIGN §8: a read-only base with your edits on top; QBX-278).
//
//   set-sampler-library    `path` = a NassauDigital program file (the library).
//                          The instance plays that program as a read-only BASE
//                          with the user's OVERLAY on top. On an instance that
//                          is already a library the overlay is KEPT and only
//                          the base is replaced (a newer version of the
//                          library): what changed under an edit is reported
//                          drifted, the overlay's value still plays. On any
//                          other instance the program is replaced by the base
//                          with an empty overlay (the undo puts it back). The
//                          file is read at the FIRST apply and remembered, so
//                          a redo installs the same text. The file is never
//                          written.
//   rebase-sampler-library Accept the current base (the core's rebaseOverlay):
//                          the overlay's baseVersion becomes the base's
//                          libraryVersion and every drifted edit's `.was` the
//                          value the base has under it, so the report clears;
//                          what plays does not change. Refused on an instance
//                          that is not a library.
//   restore-sampler-state  `library` = 1 | 0, then `base` + `overlay` (1) or
//                          `program` (0), each the core's text. Puts the
//                          instance's whole program state back: the inverse of
//                          the two verbs above, of set-pad-key on a library
//                          instance and, since QBX-282, of every program verb
//                          there and of flatten (DESIGN §8: undo removes the edit and
//                          writes the previous value back — restoring the
//                          overlay as it was before the edit is exactly that).
//                          Its own inverse is itself with the state it
//                          replaced.
//
//   hide-pad               `n` (0..95; REQUIRED). Hides pad n's BASE <Pads> row
//                          from what plays, as an overlay edit (the core's
//                          hide-pad, the author's decision of 2026-11-04):
//                          the pad goes back to its default key 36 + n, or a
//                          pad 92..95 ceases to exist. Only on a library
//                          instance. Inverse restore-sampler-state. REFUSED:
//                          not a library, no n, n outside 0..95, a pad with no
//                          base row, a row that does not play now (already
//                          hidden, or set-pad-key put it on its default).
//
//   flatten-sampler-library  Turn a library instance into a program of its
//                          own (QBX-282, the author's answer of 2026-11-07;
//                          the core's flatten()): it plays what the library
//                          played, keeps the overlay's ids (C8: never
//                          renumbered, the round-robin hashes them) and no
//                          libraryVersion; orphaned, refused and unknown edits
//                          are dropped, as flatten drops them (announced). A
//                          link only an orphaned edit named is no longer held
//                          (nothing names it). Inverse restore-sampler-state:
//                          the library, base and overlay byte for byte.
//                          REFUSED on an instance that is not a library.
//
// set-pad-key on a library instance (ssamplerrecordactions.h) records the
// core's overlay verb set-pad instead of editing the base, and since QBX-282
// every other program verb records its overlay edit too (ssampleractions.h).

#include "app/objects/sampler/ssampleractions.h"

class SSetSamplerLibraryAction : public SSamplerAction {
public:
    SSetSamplerLibraryAction() = default;
    QString name() const override { return QStringLiteral( "set-sampler-library" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    QStringList knownAttributes() const override;

private:
    QString     path_;
    // The file's text as the FIRST apply read it, so a redo installs the very
    // same base. Not serialized: a property of the moment.
    std::string baseText_;
    bool        read_ = false;
};

class SRebaseSamplerLibraryAction : public SSamplerAction {
public:
    SRebaseSamplerLibraryAction() = default;
    QString name() const override { return QStringLiteral( "rebase-sampler-library" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    QStringList knownAttributes() const override;
};

class SHidePadAction : public SSamplerAction {
public:
    SHidePadAction() = default;
    QString name() const override { return QStringLiteral( "hide-pad" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    QStringList knownAttributes() const override;

private:
    int  n_    = -1;
    bool hasN_ = false;
};

class SFlattenSamplerLibraryAction : public SSamplerAction {
public:
    SFlattenSamplerLibraryAction() = default;
    QString name() const override { return QStringLiteral( "flatten-sampler-library" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    QStringList knownAttributes() const override;
};

class SRestoreSamplerStateAction : public SSamplerAction {
public:
    SRestoreSamplerStateAction() = default;
    SRestoreSamplerStateAction( const QString &trackPath, int slotIndex, bool library,
                                const std::string &program, const std::string &base,
                                const std::string &overlay )
        : SSamplerAction( trackPath, slotIndex ), library_( library ), program_( program ),
          base_( base ), overlay_( overlay ) {}
    QString name() const override { return QStringLiteral( "restore-sampler-state" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    QStringList knownAttributes() const override;

private:
    bool        library_ = false;
    std::string program_;
    std::string base_;
    std::string overlay_;
};

#endif
