#include "app/theme/stheme.h"

#include "app/theme/sbrownprostyle.h"

#include <QApplication>
#include <QByteArray>
#include <QStyle>
#include <QtGlobal>

#include "tw/core/twlog.h"

namespace {
const char *const kBrownPro = "brownpro";
const char *const kSystem   = "system";
}

QStringList STheme::available()
{
    return QStringList{ QString::fromLatin1( kBrownPro ),
                        QString::fromLatin1( kSystem ) };
}

QString STheme::resolve( const QString &configured, bool testCase )
{
    QString name = testCase ? QString::fromLatin1( kSystem )
                            : QString::fromLatin1( kBrownPro );

    if( !configured.trimmed().isEmpty() )
        name = configured.trimmed().toLower();

    if( const QByteArray env = qgetenv( "SMARAGD_UI_THEME" ); !env.isEmpty() )
        name = QString::fromUtf8( env ).trimmed().toLower();

    if( !available().contains( name ) ) {
        TW_LOGW( "ui.theme", "unknown theme '%s'; falling back to '%s' "
                             "(known: %s)",
                 name.toUtf8().constData(),
                 testCase ? kSystem : kBrownPro,
                 available().join( QStringLiteral( ", " ) ).toUtf8().constData() );
        name = testCase ? QString::fromLatin1( kSystem )
                        : QString::fromLatin1( kBrownPro );
    }
    return name;
}

QString STheme::apply( QApplication &app, const QString &name )
{
    const QString want = available().contains( name )
        ? name
        : QString::fromLatin1( kBrownPro );

    if( want == QLatin1String( kSystem ) ) {
        TW_LOGI( "ui.theme", "theme=system; keeping the platform style '%s'",
                 app.style() ? app.style()->objectName().toUtf8().constData() : "?" );
        return want;
    }

    // QApplication TAKES OWNERSHIP of the style, so this is not a leak; and it
    // must be set before the first widget is constructed, or every widget built
    // ahead of it is polished twice (once by the outgoing style and once by
    // this one) and some keep the first one's palette.
    app.setStyle( new SBrownProStyle() );
    TW_LOGI( "ui.theme", "theme=%s installed", want.toUtf8().constData() );
    return want;
}
