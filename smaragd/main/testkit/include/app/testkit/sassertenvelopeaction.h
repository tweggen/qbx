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
 *   <assert-envelope mode="childSum" trackPath="0" start="0" length="192000"
 *                    width="64" column="10" min="-60" max="60"/>
 *
 * Parameters:
 * - clip:       index-path to the clip's SLink (the spelling slip-clip and
 *               assert-clip-channels use)
 * - mode:       "clip" (default) reads ONE clip's own probes through its
 *               renderer; "childSum" reads the FOLDER-SUM OVERLAY of the track
 *               at `trackPath` - STrack::collectChildSumEnvelope(), the exact
 *               call STrackRendererInline::draw() makes to paint it. Any other
 *               value is REJECTED rather than silently treated as clip.
 * - trackPath:  index-path to a LANE (childSum mode only), the spelling
 *               set-track-volume and assert-track-head use
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
                 QStringLiteral( "snapshot" ),  QStringLiteral( "compareTo" ),
                 QStringLiteral( "trackPath" ) };
    }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString  clipPath_;
    QString  trackPath_;
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

/**
 * `assert-lane-overlay` - the PIXEL gate on the folder-sum overlay
 * (proposal 39 M3, AC M3.10).
 *
 * assert-envelope proves the NUMBERS; this proves they reached the screen, and
 * in the right colour. It grabs the real arranger canvas off screen
 * (SMainWindow::grabArrangerLanes' machinery), finds the named lane's own band
 * - inside the two separator lines the canvas draws - and classifies every
 * pixel in it against two references:
 *
 *   the LANE FILL   (STrackRendererInline::laneFillColor, i.e. what the
 *                    renderer itself would fill with, never a guess read off
 *                    the image), and
 *   the CLIP BODY   (QColor(160,160,160), the grey a clip is drawn in).
 *
 * An OVERLAY pixel is one strictly LIGHTER than the fill and strictly DARKER
 * than the clip body - which is design D4's relation, stated as a measurement.
 * Draw the overlay darker than the lane, or as light as a clip, and the count
 * goes to zero and this fails; the two wrong directions are counted separately
 * (`darkerThanFill`, `lighterThanClip`) so the failure says which way it went.
 *
 * This closes part of the gap CLAUDE.md records - the `screenshot` verb grabs
 * a root window that is blank under QT_QPA_PLATFORM=offscreen, so before this
 * nothing gated the arranger CANVAS's paint at all.
 *
 * Run it with the time grid OFF (`grid-disable`): a grid line crosses every
 * lane and would land in the histogram as chrome.
 *
 * XML format:
 *   <assert-lane-overlay trackPath="0" grabWidth="900" grabHeight="300"
 *                        minPixels="200" grabPng="folder.png"/>
 *   <assert-lane-overlay trackPath="1" expectOverlay="false"/>
 *
 * Parameters:
 * - trackPath:     index-path to the lane (default "0")
 * - expectOverlay: "true" (default) requires overlay pixels; "false" requires
 *                  NONE, which is how a non-folder lane is gated
 * - minPixels:     how many overlay pixels are enough (default 1). A waveform
 *                  over material fills hundreds; one is the honest floor for
 *                  "it drew"
 * - grabWidth,
 *   grabHeight:    the canvas size to grab at (0 = whatever the hidden layout
 *                  gave it, which is not a picture of anything - pass them)
 * - grabPng:       also save the grab under this name in the test output dir.
 *                  Coverage, never an oracle
 * - contains:      optional substring of the report line, for a claim this
 *                  verb has no dedicated attribute for
 * - bandOnly:       "false" (default) scans the WHOLE lane strip, exactly as
 *                  before proposal 40 M2 — every existing caller is
 *                  byte-for-byte unaffected. "true" restricts the y-scan to
 *                  the BOTTOM `max(2, laneHeight/5)` rows only — the exact
 *                  same integer formula, over the exact same rect
 *                  (laneTop()+1 .. +laneHeight()-2), that
 *                  STrackRendererInline::drawFeelFlowBand uses for its own
 *                  band (proposal 40 M2).
 * - maxPixels:     ceiling for the "expectOverlay=false" case, default -1
 *                  (unset, meaning the ceiling is exactly 0 -- the ORIGINAL
 *                  behaviour, unchanged for every existing caller). Added
 *                  alongside `bandOnly` for the same reason: a lane holding
 *                  a CLIP is not a clean background for this verb even
 *                  OUTSIDE any real overlay. Measured on a real fixture, a
 *                  freshly-added, never-analyzed clip already contributes
 *                  ~1100-1200 pixels in the "strictly between fill and clip
 *                  body" luminance range -- the clip's own waveform paint
 *                  (its anti-aliased line edges), present EVEN WHEN
 *                  `bandOnly="true"` scopes the scan to the exact rows a
 *                  feel-flow band would occupy, because a waveform's peaks
 *                  and troughs reach the bottom of the clip rect as often
 *                  as the top. folder_sum_preview.qxa's own "NOT gated" note
 *                  names the same hazard and works around it by using only
 *                  CLIP-FREE lanes for its expectOverlay="false" controls;
 *                  proposal 40's feature cannot do that, because its entire
 *                  subject IS a clip-covered lane. `maxPixels` lets a case
 *                  state the intrinsic noise floor explicitly (measured,
 *                  with margin) rather than pretending the classifier can
 *                  reach a literal zero over real waveform content, while
 *                  still catching a genuine leak of feel-flow band pixels
 *                  (measured: ~1100 baseline noise vs ~8800 when the band is
 *                  actually fresh -- an 8x gap, nowhere near the ceiling).
 */
class SAssertLaneOverlayAction : public SAction
{
public:
    SAssertLaneOverlayAction() = default;

    QString name() const override { return QStringLiteral( "assert-lane-overlay" ); }
    QStringList knownAttributes() const override
    {
        return { QStringLiteral( "trackPath" ),  QStringLiteral( "expectOverlay" ),
                 QStringLiteral( "minPixels" ),  QStringLiteral( "grabWidth" ),
                 QStringLiteral( "grabHeight" ), QStringLiteral( "grabPng" ),
                 QStringLiteral( "contains" ),   QStringLiteral( "bandOnly" ),
                 QStringLiteral( "maxPixels" ) };
    }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString trackPath_     = QStringLiteral( "0" );
    bool    expectOverlay_ = true;
    int     minPixels_     = 1;
    int     grabWidth_     = 0;
    int     grabHeight_    = 0;
    QString grabPng_;
    QString contains_;
    bool    bandOnly_      = false;
    int     maxPixels_     = -1;   // -1 = unset -> ceiling is exactly 0 (old behaviour)
};

#endif // SASSERTENVELOPEACTION_H
