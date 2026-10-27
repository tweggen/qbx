#ifndef SSAMPLERTESTACTIONS_H
#define SSAMPLERTESTACTIONS_H

#include "app/actions/saction.h"

#include <QString>

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

#endif
