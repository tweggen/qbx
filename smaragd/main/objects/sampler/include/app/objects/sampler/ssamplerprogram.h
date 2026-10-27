#ifndef _SSAMPLERPROGRAM_H_
#define _SSAMPLERPROGRAM_H_

#include <QString>
#include <QVector>

#include <cstdint>
#include <functional>
#include <memory>

#include "app/model/sobject.h"
#include "tw/sampler/twsamplerprogrammodel.h"

class QDomElement;
class QTextStream;
class SObjectRenderer;
class SProject;
class SProjectLoader;
class twComponent;

namespace audio {
class twPlugin;
class twSamplerProgram;
}

/**
 * SSamplerProgram — the model object of ONE NassauDigital instance (proposal
 * 56 §4.4, Q1; nassau-digital DESIGN §9.2).
 *
 * It WRAPS THE CORE'S PROGRAM: the truth is an audio::twSamplerProgramModel
 * (tw/sampler's pimpl over nd::Program), so there is one model, Qt-free inside,
 * and this object adds what the app needs outside — undo (through the verbs in
 * ssampleractions.h), persistence, signals, and the hand-over to the instrument.
 * qbx never reads a program attribute itself (proposal 56 invariant 8).
 *
 * WHERE IT LIVES (B4): an SObject child of the SPluginSlot that hosts the
 * instance, through the slot's SLink-children loop. objects/sampler does NOT
 * depend on objects/track (Q-6, answered 2026-10-27): the slot only knows it
 * has an SObject child, and the SHELL — the one module that sees both — finds
 * the program for a slot (ssamplerslots.h) and hands the slot this object's
 * instanceInitializer() (setInstaller below).
 *
 * HOW AN EDIT REACHES THE INSTRUMENT: every committed edit makes a new
 * immutable snapshot and hands an initializer that installs it to the bound
 * installer, which (shell) sets it as the slot's instance initializer — so a
 * REBUILT instance is born current (invariant 4) — and applies it to the
 * instances that exist, under the processor's mutex (invariant 3, §4.3).
 *
 * THE SELECTION (select-zones) is model state of this object, NOT of the core
 * program: it changes no sound, installs nothing, and is NOT saved. ASSUMPTION
 * awaiting the author (proposal 56 §11, Q1): DESIGN.md and the proposal say
 * select-zones is an undoable action and no more.
 *
 * A PROGRAM THE CORE REFUSES ON LOAD (a value out of range from a newer build,
 * say) is kept VERBATIM and written back unchanged; the instance plays the
 * empty program and every edit is refused, announced. Losing the user's
 * program on the next save would be worse than not playing it.
 *
 * THE FILE: `<SSamplerProgram id='…' …><program><![CDATA[<NassauDigitalProgram
 * …>…</NassauDigitalProgram>]]></program></SSamplerProgram>`. The core's element
 * is nested verbatim as CHARACTER DATA rather than as child elements, because
 * the project file is read with QDom, and QDom does not keep attribute order —
 * QDomElement::save() writes them SORTED BY NAME (measured, Qt 6): handed back
 * to the core through QDom, a program with two unknown attributes on one
 * element would come back REORDERED, which the core's structural equality
 * (Extras in document order) calls a different program. As text, the core
 * sees exactly what it wrote (GATES Q1.2, proposal 56 §11 Q1).
 */
class SSamplerProgram : public SObject {
    Q_OBJECT

public:
    using InstanceInitializer = std::function<void( audio::twPlugin & )>;
    // What the shell binds: called with the initializer that installs the
    // current snapshot, after every committed edit and once at bind time.
    using Installer = std::function<void( const InstanceInitializer & )>;

    explicit SSamplerProgram( SProject *project );
    ~SSamplerProgram() override;

    // --- SObject: it produces no audio and is never placed on a lane ---------
    std::shared_ptr<twComponent> getRootComponent() override { return nullptr; }
    QWidget *getDetailEditWidget( QWidget * ) override { return nullptr; }
    QWidget *getInlineEditWidget( QWidget * ) override { return nullptr; }
    SObjectRenderer *getInlineRenderer() override { return nullptr; }
    // No time extent: a program is not material on a timeline.
    bool     hasDuration() const override { return false; }
    length_t getDuration() const override { return 0; }

    int serialize( QTextStream &o ) override;
    static SLink *instantiateFromDomElement( SProjectLoader &loader, QDomElement &element,
                                             SObject *parent );

    // --- the program ------------------------------------------------------------
    const audio::twSamplerProgramModel &model() const { return model_; }
    // False when the stored program could not be read; then loadError() says
    // why, every edit is refused and the stored text is written back unchanged.
    bool    isReadable() const { return loadError_.isEmpty(); }
    QString loadError() const { return loadError_; }

    // The ONE way to change the program. `fn` edits a copy; when it returns
    // true the copy replaces the program and is published (one snapshot, one
    // install); when it returns false nothing changed. Refused (false, and
    // `why`) on an unreadable program.
    bool edit( const std::function<bool( audio::twSamplerProgramModel & )> &fn,
               QString *why = nullptr );

    // --- the selection (select-zones) ----------------------------------------------
    // The ids that are selected AND exist, in selection order. A removed zone
    // drops out by itself and comes back with its undo.
    QVector<std::uint32_t> selectedZones() const;
    // The stored list, ids that no longer exist included (an inverse restores it).
    QVector<std::uint32_t> rawSelection() const { return selection_; }
    void setSelection( const QVector<std::uint32_t> &ids );

    // --- the hand-over to the instrument -----------------------------------------
    InstanceInitializer instanceInitializer() const;
    // Bind: store the installer and publish the current program through it at
    // once. An empty installer unbinds.
    void setInstaller( Installer installer );
    bool isBound() const { return static_cast<bool>( installer_ ); }
    // The serial of the snapshot most recently published (0 = none yet): what an
    // instance's twSamplerHostStats::installedSerial must equal.
    std::uint64_t publishedSerial() const { return publishedSerial_; }

signals:
    void programChanged();
    void selectionChanged();

private:
    void publish_();

    audio::twSamplerProgramModel                  model_;
    QString                                       loadError_;
    QString                                       rawText_;     // verbatim, when unreadable
    QVector<std::uint32_t>                        selection_;
    std::shared_ptr<const audio::twSamplerProgram> snapshot_;
    std::uint64_t                                 publishedSerial_ = 0;
    Installer                                     installer_;
};

#endif
