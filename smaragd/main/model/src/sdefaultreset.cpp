#include "app/model/sdefaultreset.h"

#include <QAbstractSpinBox>
#include <QEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QObject>
#include <QWidget>

namespace {

class SDoubleClickResetFilter : public QObject {
public:
    SDoubleClickResetFilter( QWidget *owner, std::function<void()> restore )
        : QObject( owner ), restore_( std::move( restore ) )
    {
    }

protected:
    bool eventFilter( QObject *, QEvent *ev ) override
    {
        if( ev->type() != QEvent::MouseButtonDblClick ) return false;
        QMouseEvent *me = static_cast<QMouseEvent *>( ev );
        if( me->button() != Qt::LeftButton ) return false;
        if( restore_ ) restore_();
        return true;   // swallowed: see the header's note on the second press
    }

private:
    std::function<void()> restore_;
};

}  // namespace

void sdefaultreset::onDoubleClick( QWidget *w, std::function<void()> restore )
{
    if( !w || !restore ) return;

    SDoubleClickResetFilter *f =
        new SDoubleClickResetFilter( w, std::move( restore ) );
    w->installEventFilter( f );

    // A spin box's text area is a child QLineEdit and it eats the double-click
    // before the spin box ever sees it (header, second note). lineEdit() is
    // protected on QAbstractSpinBox, so the child is found by type — there is
    // exactly one.
    if( qobject_cast<QAbstractSpinBox *>( w ) ) {
        if( QLineEdit *le = w->findChild<QLineEdit *>() ) le->installEventFilter( f );
    }
}
