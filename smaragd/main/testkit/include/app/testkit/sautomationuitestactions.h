#ifndef SAUTOMATIONUITESTACTIONS_H
#define SAUTOMATIONUITESTACTIONS_H

#include "app/actions/saction.h"
#include "tw/core/twtypes.h"

#include <QList>
#include <QString>
#include <Qt>

/**
 * AUTOMATION UI TEST VERBS (proposal 37 P6, design §3.4 / §6.4).
 *
 * Two of them, and they are deliberately shaped after two DIFFERENT existing
 * verbs because they test two different things:
 *
 *   drag-automation-point   the `drag-clip-edge` twin. It works out where the
 *                           addressed breakpoint IS on screen and sends REAL
 *                           press/move/release events into the arranger canvas,
 *                           so the gesture arithmetic under test is the one a
 *                           pointer drives. It is not undoable itself — the
 *                           gesture submits its OWN verb (add / move / remove /
 *                           set-automation-points), and that is what
 *                           `<undo count="1"/>` reverses.
 *
 *   automation-write-tick   the `slip-clip` shape: ONE live tick of a
 *                           Touch/Latch/Write pass. It feeds the recorder and
 *                           pushes NO undo step, exactly as a fader moving
 *                           during a pass does — the pass commits ONE
 *                           `set-automation-points` when the transport stops
 *                           (or the control is released), and that single
 *                           action is the undo step. A verb that submitted an
 *                           action per tick would test something the product
 *                           deliberately does not do.
 *
 * Both address a lane exactly as every automation verb does: `owner` is an
 * index path from the root mixer and the TARGET's space decides how it is
 * resolved (`self:` a lane, `param:` a lane plus `slotIndex`, `cut:` a
 * placement).
 */

/**
 * XML format:
 * <drag-automation-point owner="0" target="self:Volume"
 *                        time="48000" value="-6"
 *                        toTime="96000" toValue="-12" modifiers="ctrl"/>
 *
 * With `toTime`/`toValue` equal to `time`/`value` (or omitted) the gesture is a
 * plain CLICK, which on empty lane space ADDS a point and on an existing point
 * selects it. `modifiers="ctrl"` on an existing point REMOVES it; `alt` on one
 * bends its segment; `shift` on empty space rubber-bands a selection.
 *
 * The drop is PIXEL-QUANTISED and then grid-snapped, exactly like
 * drag-clip-edge and drag-note — assert a range or a snapped position, never an
 * arbitrary frame.
 */
class SDragAutomationPointAction : public SAction {
public:
    SDragAutomationPointAction() = default;

    QString name() const override
    { return QStringLiteral( "drag-automation-point" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    QStringList knownAttributes() const override;

private:
    QList<int>            ownerPath_;
    QString               target_;
    int                   slotIndex_ = -1;
    int                   take_ = -1;
    offset_t              time_ = 0;
    double                value_ = 0.0;
    offset_t              toTime_ = 0;
    double                toValue_ = 0.0;
    bool                  hasTo_ = false;
    Qt::KeyboardModifiers mods_ = Qt::NoModifier;
};

/**
 * XML format:
 * <automation-write-tick owner="0" target="self:Volume" value="-12"
 *                        time="24000"/>
 *
 * `time` is optional and defaults to the CURRENT LOCATOR, which is what a live
 * fader move does. The verb REJECTS when the addressed lane does not exist or
 * its mode is not touch/latch/write — a tick that silently did nothing would
 * make a green case prove nothing at all.
 */
class SAutomationWriteTickAction : public SAction {
public:
    SAutomationWriteTickAction() = default;

    QString name() const override
    { return QStringLiteral( "automation-write-tick" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    QStringList knownAttributes() const override;

private:
    QList<int> ownerPath_;
    QString    target_;
    int        slotIndex_ = -1;
    int        take_ = -1;
    double     value_ = 0.0;
    offset_t   time_ = 0;
    bool       hasTime_ = false;
    bool       release_ = false;
};

#endif
