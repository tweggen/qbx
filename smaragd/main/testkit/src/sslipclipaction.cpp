#include "app/testkit/sslipclipaction.h"
#include "app/actions/sactionregistry.h"
#include "app/model/slink.h"
#include "app/model/splacements.h"
#include "app/model/sobjectpath.h"
#include "app/objects/cut/scut.h"
#include "app/model/sclipwindow.h"

#include <QDomElement>
#include <QDebug>

using namespace strackpath;

SApplyResult SSlipClipAction::apply(SProject *project)
{
    if (!project || clipPath_.isEmpty()) {
        qWarning() << "slip-clip: no clip path";
        return {false, nullptr};
    }
    if (!hasStartOffset_ && !hasSrcStart_ && !hasLoopStart_) {
        qWarning() << "slip-clip: none of startOffset/srcStart/loopStart given";
        return {false, nullptr};
    }

    SObject *mixer = splacements::rootNamed( project, pathRoot_ );
    SLink *link = mixer ? splacements::placementAt(mixer, clipPath_) : nullptr;
    if (!link) {
        qWarning() << "slip-clip: no clip at path" << qualifiedToString( pathRoot_, clipPath_ );
        return {false, nullptr};
    }
    // WHICH OBJECT, with `take=` resolved through the GENERIC take seam, the
    // same one `assert-clip-window take=` uses (plan 53 A0a, QBX-177).
    //
    // Without it this verb could only reach a PLAIN clip: on a direct column
    // the placement's object is the `STakeStack`, which is not an `SCut`, so
    // the cast below refused and said so. And a take is exactly where this
    // verb is needed most — it is the only way to test the slip setters' OWN
    // invalidation (see the header), and a take column's per-take forwarding
    // is the one invalidation path that has nothing else standing in for it.
    SObject *target = &link->getSObject();
    if( take_ >= 0 ) {
        SClipWindow *w = target->windowTakeAt( take_ );
        if( !w ) {
            qWarning() << "slip-clip: no take" << take_ << "at path"
                       << qualifiedToString( pathRoot_, clipPath_ );
            return {false, nullptr};
        }
        target = &w->asObject();
    }
    SCut *cut = dynamic_cast<SCut *>( target );
    if (!cut) {
        qWarning() << "slip-clip: target is not an SCut at path"
                   << qualifiedToString( pathRoot_, clipPath_ )
                   << "take" << take_;
        return {false, nullptr};
    }

    if (hasStartOffset_) cut->setStartOffset(startOffset_);
    if (hasSrcStart_)    cut->setSrcStart(srcStart_);
    if (hasLoopStart_)   cut->setLoopStart(loopStart_);

    // No inverse: the live drag does not push one either — its release commits
    // the whole window through SResizeClipAction, and that is the undo step.
    return {true, nullptr};
}

void SSlipClipAction::writeXml(QDomElement &elem) const
{
    elem.setAttribute("clip", qualifiedToString( pathRoot_, clipPath_ ));
    if (take_ >= 0) elem.setAttribute("take", take_);
    if (hasStartOffset_)
        elem.setAttribute("startOffset", QString::fromStdString(
                              Fraction(startOffset_, 1).toString()));
    if (hasSrcStart_)
        elem.setAttribute("srcStart",
                          QString::fromStdString(srcStart_.toString()));
    if (hasLoopStart_)
        elem.setAttribute("loopStart", QString::fromStdString(
                              Fraction(loopStart_, 1).toString()));
}

bool SSlipClipAction::readXml(const QDomElement &elem, int /*version*/)
{
    clipPath_ = parseInto( pathRoot_, elem.attribute("clip") );
    if (clipPath_.isEmpty()) {
        qWarning() << "slip-clip::readXml: missing or empty clip path";
        return false;
    }
    take_ = elem.attribute("take", "-1").toInt();
    hasStartOffset_ = elem.hasAttribute("startOffset");
    if (hasStartOffset_)
        startOffset_ = (offset_t)parseFractionOrDouble(
                           elem.attribute("startOffset").toStdString()).toDouble();
    hasSrcStart_ = elem.hasAttribute("srcStart");
    if (hasSrcStart_)
        srcStart_ = parseFractionOrDouble(
                        elem.attribute("srcStart").toStdString());
    hasLoopStart_ = elem.hasAttribute("loopStart");
    if (hasLoopStart_)
        loopStart_ = (offset_t)parseFractionOrDouble(
                         elem.attribute("loopStart").toStdString()).toDouble();
    if (!hasStartOffset_ && !hasSrcStart_ && !hasLoopStart_) {
        qWarning() << "slip-clip::readXml: need one of "
                      "startOffset / srcStart / loopStart";
        return false;
    }
    return true;
}

static const bool s_reg_slip_clip = (
    SActionRegistry::instance().registerType(
        QStringLiteral("slip-clip"),
        []{ return new SSlipClipAction; }
    ), true
);
