#ifndef SSAMPLERTESTACTIONS_H
#define SSAMPLERTESTACTIONS_H

#include "app/actions/saction.h"

#include <QMap>
#include <QString>
#include <QStringList>

// sampler-test-install — give a NassauDigital slot a program and ONE sample
// from a WAV file, for the gates that need the instrument to make a sound
// before it has a model (proposal 56 Q0; the model and its verbs are Q1).
//
// A TEST VERB, NOT AN EDIT. It is not undoable and writes nothing into the
// project: in Q0 the program and the sample live in the slot's instance
// initializer and nowhere else, which is exactly what Q0.4 measures (the
// project file does not grow) and what Q0.q2 exercises (a rebuild keeps them).
// Q1's SSamplerProgram replaces it as the owner; the verb stays for gates that
// want a sample without the model.
//
// It goes through the two seams Q0 adds and nothing else:
// SPluginSlot::setInstanceInitializer() (every FUTURE instance is born with
// the install) and SPluginSlot::applyToInstances() (every CURRENT instance gets
// it under the processor's mutex, then the path above the slot is staled).
//
// Attributes: `trackPath`, `slotIndex` = "0", `filePath` (a WAV, 1 or 2
// channels, resolved like assert-audio-energy's `filename`), `layer` = "1"
// (the layer id the sample is installed for), `rootKey` = "60", `mode` =
// "oneshot" (the zone's play mode, in the core's spelling), `generation` = "1",
// and `programFile` = "" (a whole <NassauDigitalProgram> file to use INSTEAD of
// the built-in one-zone program; `rootKey`/`mode` are then ignored).
// REJECTED when the slot does not exist, is not Active, holds no
// twSamplerHost, or the file or the program cannot be read.
class SSamplerTestInstallAction : public SAction {
public:
    QString name() const override { return QStringLiteral( "sampler-test-install" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString trackPath_;
    int     slotIndex_  = 0;
    QString filePath_;
    int     layer_      = 1;
    int     rootKey_    = 60;
    QString mode_       = QStringLiteral( "oneshot" );
    qulonglong generation_ = 1;
    QString programFile_;
};

// assert-sampler-program — the Q1 gate on a NassauDigital slot's PROGRAM (proposal
// 56 §4.5; GATES Q1.1 "asserted on the program", Q1.2 "structural equality").
//
// Reads the slot's SSamplerProgram through the same resolver the verbs use
// (ssampler::programAt, never a copy of its walk) and compares with the CORE'S
// OWN structural equality (twSamplerProgramModel::operator== -> nd::Program::
// operator==). Every check is opt-in; an element that checks nothing is
// REJECTED, so a typo cannot make an assertion vacuous.
//
//   trackPath, slotIndex = "0"         the address, as the verbs take it
//   exists = "1" | "0"                 a program object is (not) there; "0" is
//                                      the only check allowed on a slot without one
//   readable = "1" | "0"               the stored program could (not) be read
//   zones = N, zoneIds = "17;18"       the zone count / the ids in program order
//   nextId = N
//   equalsFile = path                  structurally equal to that program file
//   capture = name                     remember the program under `name` (a check
//                                      of its own: it always passes)
//   equalsCaptured = name              structurally equal to what was captured,
//   ignoreNextId = "1"                 ...nextId excepted (an undone add-zone keeps
//                                      nextId, DESIGN §2 -- pair it with nextId=)
//   differsFromCaptured = name         NOT equal: proves an edit changed something,
//                                      so an equality after its undo is not vacuous
//   scope = program|zone|layer, id, attr, value
//                                      one parameter's value as the core writes it
//   xmlContains = text                 a substring of the program as the core writes it
//   selected = "3;7" (or "")           the selection: existing ids, in order
//   installed = "1"                    EVERY live instance holds the program last
//                                      published (twSamplerHostStats::installedSerial
//                                      == SSamplerProgram::publishedSerial, non-zero,
//                                      at least one instance)
class SAssertSamplerProgramAction : public SAction {
public:
    QString name() const override { return QStringLiteral( "assert-sampler-program" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    QStringList knownAttributes() const override;

private:
    QString trackPath_ = QStringLiteral( "0" );
    int     slotIndex_ = 0;
    QMap<QString, QString> checks_;   // every other attribute, by name
};

// Proposal 56 Q2 (QBX-263): the generation test verbs.

// set-sampler-render-gate — the NEXT generation job of one layer holds before
// it decodes (the set-render-gate pattern): until ready="true", or for holdMs.
// settleBoundMs= also sets the offline render's settle bound for the rest of
// the run (-1 restores SMARAGD_SAMPLER_SETTLE_MS / 30 s).
class SSetSamplerRenderGateAction : public SAction {
public:
    QString name() const override { return QStringLiteral( "set-sampler-render-gate" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    QStringList knownAttributes() const override;

private:
    QString trackPath_ = QStringLiteral( "0" );
    int     slotIndex_ = 0;
    int     layer_     = 0;
    bool    ready_     = false;
    int     holdMs_    = -1;
    bool    hasBound_  = false;
    int     settleBoundMs_ = -1;
};

// assert-sampler-layer — one layer's generation state, as the model holds it:
// generation= (installed), rendering= (1/0: a job outstanding),
// pending= (the outstanding job's generation), installs= (how many
// generations were installed), src= (what the installed buffer came from),
// captureTrackEpoch= / trackEpochAdvancedSince= (the TRACK's content epoch,
// remembered under a name / now above the remembered one: the install staled
// every page stamped before it).
class SAssertSamplerLayerAction : public SAction {
public:
    QString name() const override { return QStringLiteral( "assert-sampler-layer" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    QStringList knownAttributes() const override;

private:
    QString trackPath_ = QStringLiteral( "0" );
    int     slotIndex_ = 0;
    int     layer_     = 0;
    QMap<QString, QString> checks_;
};

// wait-sampler-generations — the settle the run barrier makes, without a
// render: waits synchronously (bounded) for every outstanding job of one
// slot's program and installs the results. Fails when one is still
// outstanding after timeoutMs.
class SWaitSamplerGenerationsAction : public SAction {
public:
    QString name() const override { return QStringLiteral( "wait-sampler-generations" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    QStringList knownAttributes() const override;

private:
    QString trackPath_ = QStringLiteral( "0" );
    int     slotIndex_ = 0;
    int     timeoutMs_ = 15000;
};

#endif
