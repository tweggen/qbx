#ifndef SDISSOLVEARRANGEMENTACTION_H
#define SDISSOLVEARRANGEMENTACTION_H

#include "app/actions/saction.h"
#include <QString>

// Action: bring an arrangement's tracks back into the master, removing its
// asset, its snippets and its placements, and unregistering the now-empty root.
// The exact inverse of extract-arrangement, and a first-class user verb in its
// own right ("put these back in the song") rather than a private undo helper.
//
//   <dissolve-arrangement name="Drums"/>
//
// restorePlan is how an UNDO puts the tracks back exactly where they came from:
// one "parentPath@index" per track, in the arrangement's own track order,
// semicolon-separated ("@" rather than ":" so it cannot be read as a root
// qualifier). It is written by extract-arrangement when it builds this as its
// inverse. Invoked by a user with no plan, the tracks are APPENDED to the
// master instead — which is the honest answer when nothing recorded where they
// used to be.
//
// THE INVERSE re-extracts, and CARRIES the window the dissolve found
// (QBX-270): "range" over the asset named after the arrangement, or "none"
// when there is no such asset (an arrangement made by create-arrangement), so
// the undo makes no asset and no placement the arrangement never had. With no
// such asset and no tracks either, the inverse is create-arrangement.
//
// REFUSED above one placement of the asset. extract-arrangement creates exactly
// one, so as an inverse this can never fire; but a user who placed the asset a
// second time and then dissolved would otherwise get one placement silently
// restored and the rest deleted. Removing the extra placements first is an
// explicit act.
class SDissolveArrangementAction : public SAction {
public:
    SDissolveArrangementAction() = default;
    explicit SDissolveArrangementAction( const QString &name,
                                         const QString &restorePlan = QString() );

    QString name() const override { return QStringLiteral("dissolve-arrangement"); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    QStringList knownAttributes() const override
    { return { QStringLiteral("name"), QStringLiteral("restorePlan") }; }

private:
    QString arrName_;
    QString restorePlan_;
};

#endif // SDISSOLVEARRANGEMENTACTION_H
