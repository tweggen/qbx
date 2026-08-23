#ifndef _SASSERTHOVERMODEACTION_H_
#define _SASSERTHOVERMODEACTION_H_

#include "app/actions/saction.h"
#include <QString>

/**
 * `assert-hover-mode` — WHAT THE APP SAYS IS UNDER THE POINTER.
 *
 * Sends ONE synthesized `MouseMove` at a clip's grab point — the SAME point
 * `drag-clip-edge` grabs, through the same `grabPointFor()`, so a hover cannot
 * be testing a box a drag would miss — and asserts the mode string the
 * arranger publishes for it (`SApplication::setStatusMode`).
 *
 * The mode string and the CURSOR SHAPE are chosen together, in one branch of
 * `updateHoverCursor`, so asserting the string is what makes the cursor
 * gateable at all: a `QCursor` set on an off-screen widget is not something a
 * headless run can read back, and the arranger's own hover feedback has never
 * had a gate of any kind.
 *
 * No button is down, so nothing arms and nothing is edited.
 */
class SAssertHoverModeAction : public SAction
{
public:
    SAssertHoverModeAction() {}
    SApplyResult apply( SProject *project ) override;
    QString name() const override
    { return QStringLiteral( "assert-hover-mode" ); }
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    int track_ = 0;
    int clip_ = 0;
    int grabWhere_ = 2;      // same spelling as drag-clip-edge's `edge`
    QString mode_;
};

#endif // _SASSERTHOVERMODEACTION_H_
