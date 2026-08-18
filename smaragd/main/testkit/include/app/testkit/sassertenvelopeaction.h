#ifndef SASSERTENVELOPEACTION_H
#define SASSERTENVELOPEACTION_H

#include "app/actions/saction.h"
#include "app/model/sobjectpath.h"

/**
 * `assert-envelope` - THE one way a script reads a DRAWN envelope
 * (proposal 39 M1, AC M1.5).
 *
 * It collects the probes through the object's own INLINE RENDERER
 * (SObjectRenderer::collectEnvelope), which is the same walk the arranger
 * paints: the same clip->source map, the same slip offset, the same loop
 * tiling, the same take delegation. A verb that read SObject::getPreview()
 * directly would be a second implementation of all of that, free to agree with
 * itself while disagreeing with every pixel on screen.
 *
 * It goes out through SMainWindow for the usual reason - testkit may not
 * include app/timeline (testkit CONTRACT inv. 5) - but note that it needs
 * NEITHER an arranger NOR a painter: a collect is expressed on a TIME WINDOW
 * (SEnvelopeWindow), so nothing here constructs a QPainter it would then
 * ignore.
 *
 * XML format:
 *   <assert-envelope clip="0,0" start="0" length="192000" width="64"
 *                    column="10" min="-90" max="90" tolerance="2"/>
 *   <assert-envelope clip="0,0" start="0" length="192000" snapshot="before"/>
 *   <assert-envelope clip="0,0" start="0" length="192000" compareTo="before"/>
 *
 * Parameters:
 * - clip:       index-path to the clip's SLink (the spelling slip-clip and
 *               assert-clip-channels use)
 * - mode:       "clip" (default). M3 adds "childSum" over `trackPath`; any
 *               other value is REJECTED rather than silently treated as clip.
 * - start,
 *   length:     the TIMELINE window, in frames
 * - width:      number of probe columns (default 64)
 * - column:     which column to check against min/max (-1 = no value check)
 * - min, max:   expected signed envelope edges of that column, in the preview's
 *               own [-128,127] units
 * - tolerance:  allowed |difference| per edge (default 0 - the probes are
 *               integers, so exactness is the normal expectation)
 * - expectEmpty: "true" asserts the collect produced NOTHING - it returned
 *               false, or every probe is zero. An event clip is the intended
 *               case; a folder with no material is M3's.
 * - snapshot:   store the whole probe array under this name
 * - compareTo:  assert the array just collected is BYTE-IDENTICAL to the one
 *               stored under this name, reporting the first differing column
 *               and both values
 *
 * THE snapshot/compareTo PAIR IS THE POINT. It is what lets M2 assert "moving
 * the fader did not move one byte of the waveform" without hard-coding a single
 * expected probe - the arrays are compared to each other, so the assertion
 * survives any change to the fixture, the preview geometry or the quantisation.
 *
 * The snapshot table is process-global and lives for the run: one .qxa script
 * per process, so a name set by one action is visible to every later one and to
 * nothing else.
 */
class SAssertEnvelopeAction : public SAction
{
public:
    SAssertEnvelopeAction() = default;

    QString name() const override { return QStringLiteral( "assert-envelope" ); }
    QStringList knownAttributes() const override
    {
        return { QStringLiteral( "clip" ),      QStringLiteral( "mode" ),
                 QStringLiteral( "start" ),     QStringLiteral( "length" ),
                 QStringLiteral( "width" ),     QStringLiteral( "column" ),
                 QStringLiteral( "min" ),       QStringLiteral( "max" ),
                 QStringLiteral( "tolerance" ), QStringLiteral( "expectEmpty" ),
                 QStringLiteral( "snapshot" ),  QStringLiteral( "compareTo" ) };
    }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString  clipPath_;
    QString  mode_        = QStringLiteral( "clip" );
    int64_t  start_       = 0;
    int64_t  length_      = 0;
    int      width_       = 64;
    int      column_      = -1;
    int      min_         = 0;
    int      max_         = 0;
    int      tolerance_   = 0;
    bool     expectEmpty_ = false;
    QString  snapshot_;
    QString  compareTo_;
};

#endif // SASSERTENVELOPEACTION_H
