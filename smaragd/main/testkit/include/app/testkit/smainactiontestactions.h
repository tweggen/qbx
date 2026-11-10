#ifndef SMAINACTIONTESTACTIONS_H
#define SMAINACTIONTESTACTIONS_H

#include "app/actions/saction.h"

/**
 * QBX-295: a main-window QAction's binding, read off the action itself.
 *
 * <assert-main-action objectName="options" menuRole="preferences"
 *                     shortcut="Ctrl+," standardKey="preferences"/>
 *   objectName:  the QAction to find among the main window's children (required);
 *   menuRole:    none|text|preferences|about|quit|aboutQt|application compared against
 *                QAction::menuRole() (see roleName() for the spellings);
 *   shortcut:    a PortableText key sequence that must be among shortcuts();
 *   standardKey: preferences: every platform binding of
 *                QKeySequence::Preferences must be among shortcuts(), so the
 *                macOS Cmd-, is asserted on the platform that defines it.
 * Each check is optional. The main window is never shown in a --test-case
 * run, so this reads the action, not what the menu bar draws.
 */
class SAssertMainActionAction : public SAction {
public:
    QString name() const override { return QStringLiteral( "assert-main-action" ); }
    QStringList knownAttributes() const override;
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString objectName_;
    QString menuRole_;
    QString shortcut_;
    QString standardKey_;
};

#endif // SMAINACTIONTESTACTIONS_H
