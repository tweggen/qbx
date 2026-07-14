#ifndef SADDTAKEACTION_H
#define SADDTAKEACTION_H

#include "app/actions/saction.h"
#include "tw/core/twfraction.h"
#include "tw/graph/tw303aenv.h"
#include <QList>
#include <QString>

// Action: add a take (a media file) to a clip (proposal 17). `clip` may
// address a take stack (the take is inserted) or a plain SCut (it is first
// wrapped into a stack, becoming take 0). The new take's cut is windowed to
// the stack duration; `startOffset` slips it, `stretch`/`pitchCents` seed
// its grain params (also used to restore a removed take's window on undo).
// `index` = -1 appends; `activate` = 1 (default) makes the new take audible
// (decision 1: newest take auto-activated).
//
// Inverse: remove-take at the inserted index (which collapses a stack back
// to a plain cut when one take remains — undoing the wrap).
class SAddTakeAction : public SAction {
public:
    SAddTakeAction() = default;
    SAddTakeAction( const QList<int> &clipPath, const QString &filePath,
                    offset_t startOffset = 0, int index = -1,
                    bool activate = true, const Fraction &stretch = Fraction(1),
                    double pitchCents = 0.0 );

    QString name() const override { return QStringLiteral("add-take"); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QList<int> clipPath_;
    QString    filePath_;
    offset_t   startOffset_ = 0;
    int        index_ = -1;
    bool       activate_ = true;
    Fraction   stretch_ = Fraction(1);
    double     pitchCents_ = 0.0;
};

#endif // SADDTAKEACTION_H
