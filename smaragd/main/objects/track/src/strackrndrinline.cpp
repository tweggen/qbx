#include <stdio.h>
#include <qpainter.h>
#include <qobject.h>
#include <QVarLengthArray>

#include "app/model/sappcontext.h"
#include "app/model/slink.h"
#include "app/objects/track/strack.h"
#include "app/model/sproject.h"
#include "app/objects/track/sfeelflowbounce.h"   // SFeelFlowUiData
#include "app/objects/track/strackcolormodifier.h"
#include "app/objects/track/strackrndrinline.h"

QColor STrackRendererInline::laneFillColor( STrack &track )
{
    // Check if this track is selected
    bool isTrackSelected = false;
    SProject *project = SAppContext::get().getCurrentProject();
    if( project ) {
        SObject *root = project->getRootComponent();
        if( root ) {
            if( root && root->activeLane() == &track ) {
                isTrackSelected = true;
            }
        }
    }

    // Determine base color (depends on selection state)
    QColor baseColor = isTrackSelected ? QColor( 60, 90, 130 ) : QColor( 40, 70, 100 );

    // Apply track state modifiers (muted, solo, armed for recording)
    STrackColorModifier mod = STrackColorModifier::fromTrackState( track );
    return mod.apply( baseColor );
}

// THE FOLDER-SUM OVERLAY (proposal 39 M3, design D4).
//
// A folder lane used to be a blank rectangle: the clip loop below skips every
// child track (correctly - a child is its own lane), so collapsing a folder made
// the arrangement under it disappear from the screen entirely. This paints the
// summed envelope of everything below it, faintly, on the lane background.
//
// It is drawn HERE and not at the canvas's own seam (SMVActualView::paintEvent),
// because this renderer's fillRect() above would paint straight over it on the
// very next line - and here it also lands in the right order: above the lane
// background, BEHIND the folder's own clips, which the loop below draws after us.
//
// The colour is DERIVED from the lane's own final colour rather than being a
// fourth hardcoded constant, so it follows selection and every
// STrackColorModifier state (muted / solo / armed) for free. Two relations
// matter and are what the gate asserts: strictly LIGHTER than the lane fill (so
// it reads as material), and DARKER than the clip body (QColor(160,160,160)
// below), so it reads as background and never competes with a real clip.
//
// Nothing here blocks: STrack::collectChildSumEnvelope() reads previews that
// already exist (main/timeline/CONTRACT.md inv. 1).
static void drawChildSumOverlay( QPainter &p, const QRect &visibRect,
                                 SRenderContext &ctx, STrack &track,
                                 const QColor &finalColor )
{
    if( !track.hasChildTracks() ) return;

    const SEnvelopeWindow win = envelopeWindowOfContext( ctx );
    const int w = win.width;
    if( w < 1 || visibRect.height() < 2 ) return;

    QVarLengthArray<preview_t> pv( w );
    for( int i = 0; i < w; ++i ) { pv[i].min = 0; pv[i].max = 0; }
    // Returns false when NOTHING contributed - a track with no child tracks, a
    // folder whose children are empty, every child inaudible - and then this
    // draws nothing at all, which is what "at least when they are non-zero"
    // asks for.
    if( !track.collectChildSumEnvelope( win, pv.data() ) ) return;

    QColor c = finalColor.lighter( 140 );
    c.setAlpha( 140 );                      // ~55 %: present, never assertive
    p.setPen( c );

    const int tl     = visibRect.topLeft().x();
    const int top    = visibRect.topLeft().y();
    const int height = visibRect.height();
    for( int i = 0; i < w; ++i ) {
        const int mn = pv[i].min;
        const int mx = pv[i].max;
        if( !mn && !mx ) continue;          // silence draws nothing, not a line
        const int y1 = top + ( ( 127 - mn ) * height ) / 256;
        const int y2 = top + ( ( 127 - mx ) * height ) / 256;
        p.drawLine( i + tl, y1, i + tl, y2 );
    }
}

// --- proposal 40 "Feel Flow" M2: the compliance heatmap band --------------
//
// Per-column tint for the compliance heatmap (design section 4.4, kickoff
// AC 2). The two endpoints of the interpolation are THIS track's own current
// fill colour (laneFillColor(), already selection/mute/solo/arm-aware) and
// the clip body's fixed grey QColor(160,160,160) -- the same pair
// drawChildSumOverlay's colour is derived from above. That choice makes the
// relation assert-lane-overlay measures (strictly lighter than the fill,
// strictly darker than the clip body) hold BY CONSTRUCTION rather than by
// eyeballing a palette: every channel is interpolated identically, and
// Rec.601 luminance is a LINEAR function of the channels, so the mixed
// colour's luminance is exactly the same linear interpolation of the two
// endpoints' luminances. For any mixing fraction strictly inside (0,1) the
// result is therefore strictly between the two -- whatever this particular
// track's fill colour happens to be. `tPercent` is kept well inside (0,100)
// (25..55, i.e. never closer than 20 to either end) purely as an extra
// margin against a dark/light STrackColorModifier state pushing the fill
// unusually close to the clip grey; the core guarantee does not need it.
//
// Mapping (a relation, not a palette -- proposal 39's own discipline, D4):
//  - LOW compliance -> closer to the clip grey (higher tPercent, higher
//    alpha): bolder and more saturated relative to the lane -- reads as
//    "hot"/flagged, a section fighting its own established feel;
//  - HIGH compliance -> closer to the fill (lower tPercent, lower alpha):
//    fainter -- reads as settled, consistent with the track's own groove.
// Alpha and tPercent move together so the two cues never disagree.
static QColor sFeelFlowTint( const QColor &fill, float compliance )
{
    compliance = qBound( 0.0f, compliance, 1.0f );
    const QColor clipBody( 160, 160, 160 );
    const int tPercent = 55 - (int) ( 30.0f * compliance );   // 55 (low) .. 25 (high)
    const int r = fill.red()   + ( ( clipBody.red()   - fill.red()   ) * tPercent ) / 100;
    const int g = fill.green() + ( ( clipBody.green() - fill.green() ) * tPercent ) / 100;
    const int b = fill.blue()  + ( ( clipBody.blue()  - fill.blue()  ) * tPercent ) / 100;
    QColor tint( qBound( 0, r, 255 ), qBound( 0, g, 255 ), qBound( 0, b, 255 ) );
    tint.setAlpha( 90 + (int) ( 120.0f * ( 1.0f - compliance ) ) );  // 210 (low) .. 90 (high)
    return tint;
}

// Drawn AFTER the clip loop, unlike the folder-sum overlay above, which is
// deliberately BEHIND clips (D4's own rationale there: a folder's own clips
// must stay on top of its child-sum hint). The heatmap targets exactly the
// clip-covered lanes this feature is about, so the "right after
// drawChildSumOverlay" slot the design's v2 text originally named would have
// painted every column invisible under a clip body -- found at kickoff,
// corrected here (section 4.4, AC 2): a bottom band, ~20% of the lane
// height, drawn LAST so it sits visibly on top of everything else.
//
// Visibility rule (AC 3): a missing OR stale track analysis paints NOTHING
// -- two cheap reads (feelFlowStale(), the atomically-cached
// feelFlowForUi()), never a demand, never a block (timeline CONTRACT inv. 1;
// feelFlowForUi()'s own doc covers the one bounded sidecar read its FIRST
// call after a fresh result may do, mirroring onsetsForUi()).
//
// Timeline frame -> hop index is PLACEMENT ARITHMETIC ONLY: the bounce is a
// whole-project render starting at project frame 0
// (sfeelflowbounce.h/.cpp's RenderParams), so its hop grid already sits in
// the SAME frame domain ctx.getTimeOf() returns here -- no warp map, no
// clip-relative mapping, anywhere in this function.
static void drawFeelFlowBand( QPainter &p, const QRect &visibRect,
                              SRenderContext &ctx, STrack &track,
                              const QColor &finalColor )
{
    if( visibRect.width() < 1 || visibRect.height() < 2 ) return;
    if( track.feelFlowStale() ) return;
    std::shared_ptr<const SFeelFlowUiData> ui = track.feelFlowForUi();
    if( !ui || ui->hopFrames == 0 || ui->compliance.empty() ) return;

    const int bandHeight = qMax( 2, visibRect.height() / 5 );   // ~20%
    const int bandTop    = visibRect.bottom() - bandHeight + 1;
    const int x0         = visibRect.left();
    const size_t nHops   = ui->compliance.size();

    for( int i = 0; i < visibRect.width(); ++i ) {
        const offset_t frame = ctx.getTimeOf( x0 + i );
        if( frame < 0 ) continue;               // before the project start
        const uint64_t hop = (uint64_t) frame / (uint64_t) ui->hopFrames;
        if( hop >= nHops ) continue;             // past the analyzed material
        p.setPen( sFeelFlowTint( finalColor, ui->compliance[hop] ) );
        p.drawLine( x0 + i, bandTop, x0 + i, visibRect.bottom() );
    }
}

/**
 * The actual track renderer function.
 * This one should render first the backings, then ask the contents
 * to render themselves into their off-screens, to then transfer them to screen.
 */
void STrackRendererInline::draw( SLink &, SRenderContext &ctx )
{
    QPainter &p = ctx.getPainter();
    QRect visibRect = ctx.getVisibRect();

    offset_t leftTime = ctx.getTimeOf( visibRect.x() );
    offset_t rightTime = ctx.getTimeOf( visibRect.x()+visibRect.width() );

    const QColor finalColor = laneFillColor( getTrack() );

    // Draw background with the final color
    p.fillRect( visibRect, finalColor );    

    // The folder's child-sum overlay goes here: after the fill (which would
    // otherwise erase it) and before the clip loop (so the folder's own clips
    // sit on top of it). Proposal 39 M3.
    drawChildSumOverlay( p, visibRect, ctx, getTrack(), finalColor );

    // BELOW the overlay on purpose. isEmpty() is childOrder_.isEmpty(), and a
    // folder's child TRACKS are child links - so it is already false for the
    // common folder and the move changes no behaviour today. It is here anyway
    // because the reading it invites ("a folder holds no clips of its own, so
    // it is empty") is the one that would silently take the overlay away, and
    // one line of ordering is cheaper than finding that out later.
    if( getTrack().isEmpty() ) {
//        p.setPen( QColor( 160, 64, 64 ) );
//        p.drawText( visibRect, AlignCenter, "Track is empty." );
        return;
    }
    // qWarning( "visibRect.x() = %d, leftTime = %d; rightTime=%d.\n", visibRect.x(), (int)leftTime, (int)rightTime );
    for( SLink *lk : getTrack().childLinks() ) {
        // Child tracks are summed into this (folder) track's audio but they are
        // their own lanes — don't draw them as clips here. The lane shows only
        // this track's own clips.
        if( dynamic_cast<STrack*>( &lk->getSObject() ) ) continue;
        bool isSelected = SAppContext::get().isSLinkSelected( lk );
        //printf( "Link found: $%08x.\n", lk );
        //fflush( stdout ); fflush( stderr );
        if( !lk->hasStartTime() ) continue;
        if( !lk->getSObject().hasDuration() ) continue;
        offset_t startTime = lk->getStartTime();
//        qWarning( "With start time of %d.\n", (int) startTime );
        length_t length = lk->getSObject().getDuration();
//        qWarning( "And length of %d.\n", (int) length );
        if( startTime >= rightTime || (startTime+length)<leftTime ) continue;
        double relStart = (double)startTime-(double)leftTime;
        double relEnd = (double)(startTime+length)-(double)leftTime;
        double startX = ((double)visibRect.x())
            +( relStart*((double)(visibRect.width()))
               / ((double)(rightTime-leftTime)) );
        if( startX-visibRect.x()>visibRect.width() ) continue;
        double endX = ((double)visibRect.x())
            +( relEnd*((double)(visibRect.width()))
               / ((double)(rightTime-leftTime)) );
        if( endX<visibRect.x() ) continue;
        p.fillRect( (int)startX, visibRect.y(),
                    (int)(endX-startX), visibRect.height(), QColor( 160, 160, 160 ) );
        if( isSelected ) {
            p.setPen( QColor( 255, 255, 255 ) );
            p.drawRect( (int)startX+1, visibRect.y()+1, 
                        (int)(endX-startX)-2, visibRect.height()-2 );
            p.setPen( QColor( 0, 0, 0 ) );
            p.drawRect( (int)startX+2, visibRect.y()+2, 
                        (int)(endX-startX)-4, visibRect.height()-4 );
        }
        // Now draw the inner of the object.
        InlineRenderContext myctx( ctx, p );
        QRect vr( (int)startX+1, visibRect.y()+1, 
                  (int)(endX-startX)-2, visibRect.height()-2 );
        if( vr.topLeft().x()<visibRect.x() ) vr.setLeft( visibRect.x() );
        if( vr.bottomRight().x()>visibRect.bottomRight().x() ) 
            vr.setRight( visibRect.bottomRight().x() );
        if( vr.width()<1 ) continue;
        myctx.setVisibRect( vr );
        //qWarning( "lk is $%08x.\n", (unsigned ) lk );
        SObjectRenderer *rndr = lk->getSObject().getInlineRenderer();
        //qWarning( "rndr is $%08x.\n", (unsigned ) rndr );        
        if( rndr ) {
            rndr->draw( *lk, myctx );
        }
        // Draw the number of links into the upper right.
        {            
            p.setPen( QColor( 0,0,0 ) );
            p.drawText( vr, Qt::AlignTop|Qt::AlignRight,
                        QString::number( lk->getSObject().getNReferences() ) );
        }
    }

    // Proposal 40 M2: the compliance heatmap band, drawn LAST -- see
    // drawFeelFlowBand()'s doc for why it cannot share the folder-sum
    // overlay's earlier slot.
    drawFeelFlowBand( p, visibRect, ctx, getTrack(), finalColor );
}

/**
 * Return the absolute time (in samples, for now) of the given x position.
 * This depends on the zoom factor of this model.
 */
offset_t STrackRendererInline::InlineRenderContext::getTimeOf( int x ) const
{
    return parentRC_.getTimeOf( x );
}

STrackRendererInline::InlineRenderContext::~InlineRenderContext()
{
}

STrackRendererInline::InlineRenderContext::InlineRenderContext( 
    SRenderContext &par, QPainter &painter )
    : SRenderContext( painter ),
      parentRC_( par )
{    
}


STrackRendererInline::STrackRendererInline( STrack &track )
    : SObjectRenderer( (SObject &)track )
{
}

STrackRendererInline::~STrackRendererInline()
{
}
