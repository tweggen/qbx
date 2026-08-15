#include "app/model/sclipwindow.h"

#include <QDebug>
#include <QHash>

// The per-contentKind wrap factory. A function-local static, like the loader's
// type registry: the registrants run from static initializers in other
// translation units, and a namespace-scope container would not reliably be
// constructed before the first of them.
static QHash<int, SClipWindow::WrapFn> &wrapFactories()
{
    static QHash<int, SClipWindow::WrapFn> factories;
    return factories;
}

void SClipWindow::registerWrapFactory( SContentKind kind, WrapFn fn )
{
    if( fn ) wrapFactories().insert( (int) kind, fn );
}

SClipWindow *SClipWindow::wrapContent( SProject *project, SObject &content )
{
    const SContentKind kind = content.contentKind();
    const WrapFn fn = wrapFactories().value( (int) kind, nullptr );
    if( !fn ) {
        qWarning() << "SClipWindow::wrapContent: no window type registered for "
                      "content kind" << (int) kind << "of"
                   << content.metaObject()->className();
        return nullptr;
    }
    return fn( project, content );
}

SClipWindow *SClipWindow::of( SObject *obj )
{
    // The ONE cross-cast in the model. Every windowed verb goes through it, so
    // "is this placement a window?" has exactly one spelling — the class-name
    // string compare it replaces (ssplitclipaction.cpp) could not have been
    // extended to a second window type at all.
    return obj ? dynamic_cast<SClipWindow *>( obj ) : nullptr;
}
