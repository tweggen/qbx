#ifndef _SASSERTCLIPMIXACTION_H_
#define _SASSERTCLIPMIXACTION_H_

#include <QString>

#include "app/actions/saction.h"

/**
 * `assert-clip-mix` - a clip's per-clip STATIC volume/pan
 * (`SObject::volume_`/`pan_`, edited via `set-clip-volume` / `set-clip-pan`).
 *
 * Complements `assert-clip-window`, which is scoped to the `SClipWindow`
 * interface (placement/window geometry) and does not reach these SObject-level
 * scalars — volume and pan are not window properties, they live on every
 * `SObject` (see `main/objects/cut/CONTRACT.md`, "Per-clip static volume and
 * pan"). Resolution mirrors `assert-clip-window`: the clip path, then the take
 * seam (`windowTakeAt`), so a stack's ACTIVE (or a NAMED) take is read exactly
 * as `set-clip-volume`/`set-clip-pan` would target it.
 *
 * Both bounds are optional (attribute absent = not checked), each with its
 * own tolerance, so a case asserts the one value it is about.
 */
class SAssertClipMixAction : public SAction
{
public:
    SAssertClipMixAction() = default;

    QString name() const override
    { return QStringLiteral( "assert-clip-mix" ); }
    QStringList knownAttributes() const override
    { return { "clip", "take", "expectVolumeDb", "volumeTolerance",
               "expectPan", "panTolerance" }; }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString clip_;
    int     take_ = -1;
    bool    hasVolume_       = false;
    double  expectVolumeDb_  = 0.0;
    double  volumeTolerance_ = 1e-6;
    bool    hasPan_          = false;
    double  expectPan_       = 0.0;
    double  panTolerance_    = 1e-6;
};

#endif // _SASSERTCLIPMIXACTION_H_
