#include "swaveformdraw.h"

#include <qpainter.h>
#include <QVarLengthArray>
#include <cmath>

#include "sobject.h"
#include "slink.h"
#include "sobjectrenderer.h"
#include "strack.h"

bool drawObjectWaveform( SObject &obj, SLink &lk, SRenderContext &ctx,
                         const QColor &waveColor )
{
    QPainter &p = ctx.getPainter();
    QRect r = ctx.getVisibRect();
    int tl     = r.topLeft().x();
    int top    = r.topLeft().y();
    int height = r.height();

    // Time span of the visible rect, in the object's own (source) domain: the
    // context maps pixel -> time, and we subtract the link start so 0 is the
    // object's origin (the cut context folds its window offset in here).
    offset_t o1 = ctx.getTimeOf( r.topLeft().x() );
    offset_t o2 = ctx.getTimeOf( r.bottomRight().x() );
    if( o2 <= o1 ) o2 = o1 + 1;
    o1 -= lk.getStartTime();
    o2 -= lk.getStartTime();

    int w = r.width();
    if( w < 1 ) w = 1;

    QVarLengthArray<preview_t> pv( w );
    int res = obj.getPreview( pv.data(), o1, o2 - o1, w );
    if( res < 0 ) return false;

    // Apply track volume scaling: convert dB to linear gain
    // Snapshot the volume to avoid race conditions with the audio thread.
    // Track parent is typically an STrack; get its volume in dB.
    double volumeGain = 1.0;
    QObject *qparent = lk.parent();
    if( qparent ) {
        if( STrack *track = dynamic_cast<STrack*>( qparent ) ) {
            // Thread-safe volume read: use mutex to prevent audio thread from
            // modifying volume during our read (audio playback + UI slider move).
            double volumeDB = 0.0;
            {
                std::lock_guard<std::mutex> lock( track->volumeMutex_ );
                volumeDB = track->getVolume();
            }
            // Convert dB to linear: gain = 10^(dB/20)
            volumeGain = pow( 10.0, volumeDB / 20.0 );
        }
    }

    p.setPen( waveColor );
    for( int i = 0; i < w; i++ ) {
        int x = i + tl;
        // Scale the preview peaks by the volume gain, clamp to [-127, 127]
        int scaledMin = (int)( pv[i].min * volumeGain );
        int scaledMax = (int)( pv[i].max * volumeGain );
        scaledMin = qBound( -127, scaledMin, 127 );
        scaledMax = qBound( -127, scaledMax, 127 );

        int y1 = top + ( ( 127 - scaledMin ) * height ) / 256;
        int y2 = top + ( ( 127 - scaledMax ) * height ) / 256;
        p.drawLine( x, y1, x, y2 );
    }
    return true;
}
