#include "app/testkit/sstatusbartestactions.h"

#include "app/actions/sactionregistry.h"
#include "app/shell/smainwindow.h"

#include <QApplication>
#include <QDebug>
#include <QDomElement>

namespace {

SMainWindow *mainWindow()
{
    for (QWidget *w : QApplication::topLevelWidgets()) {
        if (SMainWindow *win = qobject_cast<SMainWindow *>(w)) return win;
    }
    return nullptr;
}

}  // namespace

// ------------------------------------------------------- assert-status-bar

QStringList SAssertStatusBarAction::knownAttributes() const
{
    return {QStringLiteral("contains"), QStringLiteral("absent")};
}

SApplyResult SAssertStatusBarAction::apply(SProject * /*project*/)
{
    SMainWindow *win = mainWindow();
    if (!win) {
        qWarning() << "assert-status-bar: no main window";
        return {false, nullptr};
    }
    const QString got = win->describeStatusBar();
    if (!contains_.isEmpty() && !got.contains(contains_)) {
        qWarning().noquote() << "assert-status-bar FAILED: status bar reports"
                             << ("\"" + got + "\"") << "which lacks"
                             << ("\"" + contains_ + "\"");
        return {false, nullptr};
    }
    if (!absent_.isEmpty() && got.contains(absent_)) {
        qWarning().noquote() << "assert-status-bar FAILED: status bar reports"
                             << ("\"" + got + "\"") << "which contains"
                             << ("\"" + absent_ + "\"");
        return {false, nullptr};
    }
    qDebug().noquote() << "assert-status-bar: OK -" << ("\"" + got + "\"");
    return {true, nullptr};
}

void SAssertStatusBarAction::writeXml(QDomElement &elem) const
{
    if (!contains_.isEmpty()) elem.setAttribute("contains", contains_);
    if (!absent_.isEmpty())   elem.setAttribute("absent", absent_);
}

bool SAssertStatusBarAction::readXml(const QDomElement &elem, int /*version*/)
{
    contains_ = elem.attribute("contains");
    absent_   = elem.attribute("absent");
    return true;
}

static const bool s_reg_assertstatusbar = (
    SActionRegistry::instance().registerType(
        QStringLiteral("assert-status-bar"),
        []{ return new SAssertStatusBarAction; }
    ), true
);

// ---------------------------------------------------- click-solo-indicator

QStringList SClickSoloIndicatorAction::knownAttributes() const
{
    return {};
}

SApplyResult SClickSoloIndicatorAction::apply(SProject * /*project*/)
{
    SMainWindow *win = mainWindow();
    if (!win) {
        qWarning() << "click-solo-indicator: no main window";
        return {false, nullptr};
    }
    // REJECTS when the command is disabled -- see the header. The clear
    // itself goes on the real undo stack as its own macro, so this action
    // carries no inverse of its own: a `undo` here would undo the clear,
    // which is what a case wants, and an inverse here would undo it twice.
    if (!win->clickSoloIndicator()) {
        qWarning() << "click-solo-indicator: the command is disabled "
                      "(no solo is in force)";
        return {false, nullptr};
    }
    return {true, nullptr};
}

void SClickSoloIndicatorAction::writeXml(QDomElement & /*elem*/) const
{
}

bool SClickSoloIndicatorAction::readXml(const QDomElement & /*elem*/, int /*version*/)
{
    return true;
}

static const bool s_reg_clicksoloindicator = (
    SActionRegistry::instance().registerType(
        QStringLiteral("click-solo-indicator"),
        []{ return new SClickSoloIndicatorAction; }
    ), true
);
