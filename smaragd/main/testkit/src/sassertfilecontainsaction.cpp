#include "app/testkit/sassertfilecontainsaction.h"
#include "app/actions/sactionregistry.h"
#include <QDebug>
#include <QDomElement>
#include <QFile>

SAssertFileContainsAction::SAssertFileContainsAction( const QString &path,
                                                      const QString &text,
                                                      bool absent )
    : path_( path ), text_( text ), absent_( absent )
{
}

SApplyResult SAssertFileContainsAction::apply( SProject * /*project*/ )
{
    if( path_.isEmpty() || text_.isEmpty() ) {
        qWarning() << "SAssertFileContainsAction: path and text are both required";
        return { false, nullptr };
    }

    QFile f( path_ );
    if( !f.open( QIODevice::ReadOnly ) ) {
        qWarning() << "SAssertFileContainsAction: cannot open" << path_;
        return { false, nullptr };
    }
    const QString content = QString::fromUtf8( f.readAll() );
    f.close();

    const bool found = content.contains( text_ );
    if( found == absent_ ) {
        qWarning() << "SAssertFileContainsAction:" << path_
                   << ( absent_ ? "unexpectedly CONTAINS" : "does NOT contain" )
                   << text_;
        return { false, nullptr };
    }

    qDebug() << "SAssertFileContainsAction: OK —" << path_
             << ( absent_ ? "does not contain" : "contains" ) << text_;
    return { true, nullptr };   // assertions are not undoable
}

void SAssertFileContainsAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "path", path_ );
    elem.setAttribute( "text", text_ );
    if( absent_ ) elem.setAttribute( "absent", "true" );
}

bool SAssertFileContainsAction::readXml( const QDomElement &elem, int /*version*/ )
{
    path_   = elem.attribute( "path", "" );
    text_   = elem.attribute( "text", "" );
    absent_ = elem.attribute( "absent", "false" ) == "true";
    if( path_.isEmpty() || text_.isEmpty() ) {
        qWarning() << "SAssertFileContainsAction::readXml: missing path or text";
        return false;
    }
    return true;
}

static const bool s_reg_assert_file_contains = (
    SActionRegistry::instance().registerType(
        QStringLiteral("assert-file-contains"),
        []{ return new SAssertFileContainsAction; }
    ), true
);
