#ifndef SPURGEUNUSEDCLIPSTESTACTIONS_H
#define SPURGEUNUSEDCLIPSTESTACTIONS_H

#include "app/actions/saction.h"

#include <QString>
#include <QStringList>

// assert-unused-clips (QBX-319) -- THE LIST the purge acts on.
//
// Reads sunusedclips::describe(), which is built from sunusedclips::list():
// the same function the Edit menu greys on, the confirmation lists and
// `purge-unused-clips rows=` indexes. "count=N|0:<label>|1:<label>...".
//
//   count        "-1" (not checked) -- how many unused clips there are.
//   contains     ""  -- a substring the description must hold.
//   absent       ""  -- a substring it must NOT hold.
//   menuEnabled  "-1" (not checked) -- whether Edit > "Purge unused clips..."
//                is enabled, read the way a user meets it: the seam emits the
//                Edit menu's aboutToShow() and then reads the QAction.
class SAssertUnusedClipsAction : public SAction {
public:
    QString name() const override
    { return QStringLiteral( "assert-unused-clips" ); }
    QStringList knownAttributes() const override
    {
        return { QStringLiteral( "count" ), QStringLiteral( "contains" ),
                 QStringLiteral( "absent" ), QStringLiteral( "menuEnabled" ) };
    }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    int     count_ = -1;
    QString contains_;
    QString absent_;
    int     menuEnabled_ = -1;
};

// purge-unused-clips-gesture (QBX-319) -- THE PURGE AS A USER REACHES IT.
//
//   via            "rows" (default) -- SMainWindow::purgeUnusedClipsGesture
//                  with the rows supplied instead of asked for. Everything
//                  else (the list, the submit, the count) is the code the
//                  menu runs.
//                  "menu" -- trigger Edit > "Purge unused clips..." itself.
//                  The seam emits aboutToShow() first and REFUSES a disabled
//                  item (QAction::trigger() would fire it anyway). An enabled
//                  item opens the confirmation, which under --test-case
//                  declines, so this purges nothing: the gate on "nothing is
//                  released without a confirmation".
//   rows           ""  -- ';'-separated rows of the list, as ticked.
//   expectPurged   "-1" (not checked) -- EXACT number released. Zero is a
//                  real answer (declined, nothing ticked).
//   expectRefused  "0" -- "1" when the menu item must be greyed, so the
//                  seam's refusal is the expected outcome.
class SPurgeUnusedClipsGestureAction : public SAction {
public:
    QString name() const override
    { return QStringLiteral( "purge-unused-clips-gesture" ); }
    QStringList knownAttributes() const override
    {
        return { QStringLiteral( "via" ), QStringLiteral( "rows" ),
                 QStringLiteral( "expectPurged" ),
                 QStringLiteral( "expectRefused" ) };
    }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString via_ = QStringLiteral( "rows" );
    QString rows_;
    int     expectPurged_ = -1;
    bool    expectRefused_ = false;
};

#endif // SPURGEUNUSEDCLIPSTESTACTIONS_H
