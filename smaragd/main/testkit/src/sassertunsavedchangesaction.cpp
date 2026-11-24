#include "app/testkit/sassertunsavedchangesaction.h"

#include <QApplication>
#include <QDebug>
#include <QDomElement>
#include <QUndoStack>

#include "app/actions/sactionhistory.h"
#include "app/actions/sactionregistry.h"
#include "app/shell/sapplication.h"
#include "app/shell/smainwindow.h"

SApplyResult SAssertUnsavedChangesAction::apply( SProject * )
{
    SMainWindow *win = nullptr;
    for( QWidget *w : QApplication::topLevelWidgets() )
        if( ( win = qobject_cast<SMainWindow*>( w ) ) ) break;
    if( !win ) {
        qWarning() << "assert-unsaved-changes: no main window";
        return { false, nullptr };
    }

    const bool got = win->unsavedChangesForTest();
    const bool want = expect_ != 0;
    if( got != want ) {
        qWarning() << "assert-unsaved-changes FAILED: expected"
                   << ( want ? "unsaved" : "clean" ) << "got"
                   << ( got ? "unsaved" : "clean" );
        return { false, nullptr };
    }
    if( undoEntries_ >= 0 ) {
        SActionHistory *hist = SApplication::app().actionHistory();
        QUndoStack *stack = hist ? hist->undoStack() : nullptr;
        const int n = stack ? stack->count() : -1;
        if( n != undoEntries_ ) {
            qWarning() << "assert-unsaved-changes FAILED: expected"
                       << undoEntries_ << "undo entries, got" << n;
            return { false, nullptr };
        }
    }
    return { true, nullptr };
}

void SAssertUnsavedChangesAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "expect", expect_ );
    if( undoEntries_ >= 0 ) elem.setAttribute( "undoEntries", undoEntries_ );
}

bool SAssertUnsavedChangesAction::readXml( const QDomElement &elem, int )
{
    expect_      = elem.attribute( "expect", "1" ).toInt();
    undoEntries_ = elem.attribute( "undoEntries", "-1" ).toInt();
    return true;
}

static const bool s_reg_assert_unsaved_changes = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "assert-unsaved-changes" ),
        []{ return new SAssertUnsavedChangesAction; } ), true );
