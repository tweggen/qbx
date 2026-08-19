#ifndef SSETCLIPVOLUMEACTION_H
#define SSETCLIPVOLUMEACTION_H

#include "app/actions/saction.h"
#include <QList>

// Action: set a clip's STATIC volume trim, in dB (0 = unity — the default
// every SObject, and therefore every clip, is already born with; see
// SObject::volume_ / getVolume() / setVolume()). The value is ABSOLUTE (undo
// restores the exact previous number, not a delta — mirrors set-pitch /
// set-formant-preserve). Take stacks: a PER-TAKE property, same rule as
// pitch/formant (take -1 = the active take, resolved at apply time so the
// inverse can name it explicitly).
//
// HOW THIS COMPOSES WITH `cut:Gain` (the proposal 37 P5 clip automation
// envelope, mix/CONTRACT.md inv. 23): the two SUM IN dB, the same rule
// twGainStage already applies between a track's own static fader and a
// `self:Volume` lane (mix/CONTRACT.md inv. 21, "TRIM SUMS IN dB" — a dB sum
// is a product of linear factors). Concretely: twTrackMix::ClipEntry carries
// a LINEAR gainScalar alongside its existing LINEAR gainCurve, and the mix
// loop multiplies the two per frame. STrack::refreshClipGainCurves() is the
// one place that converts this dB value to that linear scalar and pushes it
// — the same main-thread funnel that already pushes the curve, so both react
// to exactly the same set of model edits (see SObject::invalidateRenderPathRange
// and STrack::bumpRenderChainEpochRange). At 0 dB with no envelope the mix
// loop takes the untouched null-scalar path, so an unedited project's render
// is unchanged byte-for-byte.
class SSetClipVolumeAction : public SAction {
public:
    SSetClipVolumeAction() = default;
    SSetClipVolumeAction( const QList<int> &clipPath, double volumeDb,
                          int take = -1, bool broadcast = true );

    QString name() const override
        { return QStringLiteral("set-clip-volume"); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

    // The clamp every entry point uses. Deliberately duplicated rather than
    // shared with app/timeline/sfadercurve.h's SFADER_MIN_DB/SFADER_MAX_DB:
    // objects/cut may not depend on the timeline module (tools/check_layering.py,
    // APP_DEPS['objects/cut']), so a shared header is not available here.
    static constexpr double VOLUME_MIN_DB = -96.0;
    static constexpr double VOLUME_MAX_DB =  24.0;
    static double clampVolumeDb( double db ) {
        if( db < VOLUME_MIN_DB ) return VOLUME_MIN_DB;
        if( db > VOLUME_MAX_DB ) return VOLUME_MAX_DB;
        return db;
    }

private:
    QList<int> clipPath_;
    double     volumeDb_  = 0.0;
    int        take_      = -1;
    bool       broadcast_ = true;
};

#endif // SSETCLIPVOLUMEACTION_H
