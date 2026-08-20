#include "app/timeline/ssubmit.h"
#include "app/timeline/sstdmixerview.h"
#include "app/shell/smainwindow.h"
#include "app/shell/sviewtabs.h"
#include "app/shell/sapplication.h"
#include "app/actions/saction.h"
#include <QApplication>

namespace {

SStdMixerView *activeArranger()
{
    for( QWidget *w : QApplication::topLevelWidgets() ) {
        if( SMainWindow *win = qobject_cast<SMainWindow *>( w ) ) {
            if( SViewTabs *tabs = win->viewTabs() )
                return dynamic_cast<SStdMixerView *>( tabs->activeEditor() );
        }
    }
    return nullptr;
}

}  // namespace

void stimeline::submitFor( SStdMixerView *v, SAction *a )
{
    if( !a ) return;
    if( v ) a->setPathRoot( v->rootName() );
    SApplication::app().submitAction( a );
}

void stimeline::submitActive( SAction *a )
{
    submitFor( activeArranger(), a );
}

const char *stimeline::rootNameDoc() { return "SStdMixerView::rootName()"; }
