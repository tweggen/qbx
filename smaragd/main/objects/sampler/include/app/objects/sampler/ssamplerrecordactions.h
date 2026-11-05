#ifndef _SSAMPLERRECORDACTIONS_H_
#define _SSAMPLERRECORDACTIONS_H_

// ssamplerrecordactions.h — recording into the instrument (proposal 56 Q6,
// §4.10; nassau-digital DESIGN §7, GATES Q6), and the instance state the
// landing rule reads.
//
//   set-sampler-view          `view` = key | pad | map | zone, `padBank`
//                             (0..5), `pad` (0..15); each optional, at least
//                             one. Absolute; the inverse is set-sampler-view
//                             with all three previous values. Brought forward
//                             from Q7 into Q6 (the author, 2026-11-02): the
//                             editor will drive this same verb.
//   set-pad-key               `n` (the pad over all banks, 0..95), `key`
//                             (0..127). Points the pad at another key in the
//                             program's <Pads> table (DESIGN §13, 2026-11-03,
//                             QBX-277): only the key changes, no zone moves;
//                             the pad's default key (36 + n) removes its row.
//                             It EDITS THE PROGRAM (it travels into an
//                             export), through SSamplerProgram::edit like the
//                             program verbs. The inverse is set-pad-key with
//                             the previous key (`key` = -1 only for a pad
//                             past the defaults, 92..95, that had no key).
//                             On a LIBRARY instance (QBX-278) it records the
//                             core's overlay verb set-pad instead and the
//                             inverse is restore-sampler-state
//                             (ssamplerlibraryactions.h).
//   set-sampler-record-input  `input` = audio:<device>:<mask> | "" (none).
//                             The instance's own record input (Q-1). Absolute;
//                             the inverse is itself with the previous value.
//   place-sampler-recording   `filePath`, `srcOffset` = 0, `length` = 0 (to the
//                             end), `target` = "" | pad | key, `key` = -1.
//                             Lands a recording in the instrument by DESIGN
//                             §7's rule (below) and registers the file as a
//                             qbx asset (Q-4). ONE undo step removes both.
//
// Both state verbs change no sound and publish nothing, and both create the
// program object on a NassauDigital slot that has none (a verb that is going
// to succeed, invariant 4), as add-zone does.
//
// WHERE A RECORDING LANDS (DESIGN §7), decided at the FIRST apply and
// remembered, so a redo lands it where it landed:
//   target   `target` when given, else the instance's view: Pad -> pad, any
//            other view -> key.
//   pad      key K = `key` when given, else the selected pad's key (the
//            program's <Pads> table; refused when the pad has none). The
//            pad's zone is THE ZONE COVERING K (keyLo <= K <= keyHi, any
//            velocity; DESIGN §13, "Settled on 2026-11-03, pads", QBX-277),
//            so several pads may share it: when exactly one zone covers K its
//            FIRST layer is replaced (src, start, end; loop points reset to
//            the whole sample and beats to 0; every other layer setting kept)
//            and every pad on that zone hears the take; when it has no layer
//            one is added. No zone covering K: a new one-key zone on K,
//            rootKey K. MORE THAN ONE (velocity layers, round-robin zones):
//            REFUSED with a reason, nothing changes.
//   key      a program with no zones: a zone over the whole keyboard (keyLo 0,
//            keyHi 127, the core's default root key, 60). A program with zones:
//            a new one-key zone on K = `key` when given, else the LAST KEY
//            PLAYED on the instance's device MIDI input (SSamplerProgram::
//            lastKey); refused when no key has been played.
// The layer's src is `file:<filePath>`, start = srcOffset, end = srcOffset +
// length (or the sample's end). The program child is add-zone, add-layer or
// set-layer; the asset is `create-file-asset`, emitted through the REGISTRY
// (objects/cut owns it; this module may not name it). Both are children of one
// composite HELD by the action, so a redo re-applies the same children, which
// put back the same ids and the same asset name.

#include "app/objects/sampler/ssampleractions.h"

class SCompositeAction;

class SSetSamplerViewAction : public SSamplerAction {
public:
    SSetSamplerViewAction() = default;
    SSetSamplerViewAction( const QString &trackPath, int slotIndex, const QString &view,
                           int padBank, int pad )
        : SSamplerAction( trackPath, slotIndex ), view_( view ), padBank_( padBank ), pad_( pad ) {}
    QString name() const override { return QStringLiteral( "set-sampler-view" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    QStringList knownAttributes() const override;

private:
    QString view_;          // empty = keep
    int     padBank_ = -1;  // -1 = keep
    int     pad_     = -1;  // -1 = keep
};

class SSetPadKeyAction : public SSamplerAction {
public:
    SSetPadKeyAction() = default;
    SSetPadKeyAction( const QString &trackPath, int slotIndex, int n, int key )
        : SSamplerAction( trackPath, slotIndex ), n_( n ), key_( key ), hasN_( true ),
          hasKey_( true ) {}
    QString name() const override { return QStringLiteral( "set-pad-key" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    QStringList knownAttributes() const override;

private:
    int  n_      = -1;
    int  key_    = -1;
    bool hasN_   = false;
    bool hasKey_ = false;
};

class SSetSamplerRecordInputAction : public SSamplerAction {
public:
    SSetSamplerRecordInputAction() = default;
    SSetSamplerRecordInputAction( const QString &trackPath, int slotIndex, const QString &input )
        : SSamplerAction( trackPath, slotIndex ), input_( input ), hasInput_( true ) {}
    QString name() const override { return QStringLiteral( "set-sampler-record-input" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    QStringList knownAttributes() const override;

private:
    QString input_;
    bool    hasInput_ = false;
};

class SPlaceSamplerRecordingAction : public SSamplerAction {
public:
    SPlaceSamplerRecordingAction() = default;
    SPlaceSamplerRecordingAction( const QString &trackPath, int slotIndex, const QString &filePath,
                                  qint64 srcOffset, qint64 length,
                                  const QString &target = QString(), int key = -1 )
        : SSamplerAction( trackPath, slotIndex ), filePath_( filePath ), srcOffset_( srcOffset ),
          length_( length ), target_( target ), key_( key ) {}
    ~SPlaceSamplerRecordingAction() override;
    QString name() const override { return QStringLiteral( "place-sampler-recording" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    QStringList knownAttributes() const override;

private:
    QString filePath_;
    qint64  srcOffset_ = 0;
    qint64  length_    = 0;
    QString target_;
    int     key_       = -1;
    // The plan the first apply built (the program child, then the asset
    // child). HELD, not stack-local: the redo re-applies these very children.
    SCompositeAction *plan_ = nullptr;
};

#endif
