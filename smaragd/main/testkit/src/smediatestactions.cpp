#include "app/testkit/smediatestactions.h"

#include "app/actions/sactionregistry.h"
#include "app/media/smediaregistry.h"
#include "app/media/smediasource.h"
#include "app/shell/sapplication.h"
#include "app/shell/smainwindow.h"
#include "app/shell/smediaaccountmanager.h"
#include "app/shell/ssettings.h"
#include "app/servicesui/soptionsdialog.h"

// Same directory as this file -- swebdavstub.h is testkit's own PRIVATE
// header (not under include/app/testkit/), so no extra include dir is
// needed the way media/tests/webdav_source_test.cpp needs one.
#include "swebdavstub.h"

#include <QApplication>
#include <QDebug>
#include <QDomElement>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QThread>
#include <QTimer>
#include <QWidget>

#include <memory>

namespace {

// The panel lives under the main window, and the drag has to reach the arranger
// as well — so every one of these hops through the shell, the same route
// drag-clip-edge and assert-lane-alignment take.
SMainWindow *mainWindow()
{
    for( QWidget *w : QApplication::topLevelWidgets() ) {
        if( SMainWindow *win = qobject_cast<SMainWindow *>( w ) ) return win;
    }
    return nullptr;
}

// Wait for the panel to go IDLE: no live root request, no pending lazy expand,
// no search still inside its debounce. NEVER a fixed sleep — the walk runs on
// app/media's private pool and finishes when it finishes, and a case that slept
// long enough on an idle box would flake under `ctest -j4`.
//
// Returns false ONLY on a timeout, which is a real failure: a request that
// never completes is exactly the bug this would be hiding.
bool waitIdle( SMainWindow *win, int timeoutMs )
{
    if( timeoutMs <= 0 ) return true;
    QElapsedTimer t;
    t.start();
    for( ;; ) {
        QCoreApplication::processEvents();
        if( !win->mediaBrowserBusy() ) return true;
        if( t.elapsed() > timeoutMs ) {
            qWarning() << "media browser: still busy after" << timeoutMs
                       << "ms:" << win->describeMediaBrowser().section( '\n', 0, 0 );
            return false;
        }
        QThread::msleep( 2 );
    }
}

}   // namespace

// --------------------------------------------------------------------------
// media-browser-source
// --------------------------------------------------------------------------

SApplyResult SMediaBrowserSourceAction::apply( SProject * /*project*/ )
{
    SMainWindow *win = mainWindow();
    if( !win ) { qWarning() << "media-browser-source: no main window"; return { false, nullptr }; }
    if( !win->mediaBrowserSetSource( sourceId_ ) ) {
        qWarning() << "media-browser-source: no source registered as" << sourceId_;
        return { false, nullptr };
    }
    if( !waitIdle( win, waitMs_ ) ) return { false, nullptr };
    return { true, nullptr };   // a test verb has nothing to undo
}

void SMediaBrowserSourceAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "sourceId", sourceId_ );
    elem.setAttribute( "waitMs", waitMs_ );
}

bool SMediaBrowserSourceAction::readXml( const QDomElement &elem, int )
{
    sourceId_ = elem.attribute( "sourceId", "local" );
    waitMs_   = elem.attribute( "waitMs", "2000" ).toInt();
    return true;
}

// --------------------------------------------------------------------------
// media-browser-path
// --------------------------------------------------------------------------

SApplyResult SMediaBrowserPathAction::apply( SProject * /*project*/ )
{
    SMainWindow *win = mainWindow();
    if( !win ) { qWarning() << "media-browser-path: no main window"; return { false, nullptr }; }

    if( !path_.isEmpty() ) {
        if( !win->mediaBrowserSetPath( path_, QString() ) ) {
            qWarning() << "media-browser-path: could not browse" << path_;
            return { false, nullptr };
        }
        // The row an expand names does not exist until the root listing has
        // landed, so the two are SEQUENCED here rather than issued together.
        if( !waitIdle( win, waitMs_ ) ) return { false, nullptr };
    }

    if( !expand_.isEmpty() ) {
        if( !win->mediaBrowserSetPath( QString(), expand_ ) ) {
            qWarning() << "media-browser-path: no directory row named" << expand_;
            return { false, nullptr };
        }
        if( !waitIdle( win, waitMs_ ) ) return { false, nullptr };
    }
    return { true, nullptr };
}

void SMediaBrowserPathAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "path", path_ );
    elem.setAttribute( "expand", expand_ );
    elem.setAttribute( "waitMs", waitMs_ );
}

bool SMediaBrowserPathAction::readXml( const QDomElement &elem, int )
{
    path_   = elem.attribute( "path" );
    expand_ = elem.attribute( "expand" );
    waitMs_ = elem.attribute( "waitMs", "2000" ).toInt();
    if( path_.isEmpty() && expand_.isEmpty() ) {
        qWarning() << "media-browser-path: needs a path= or an expand=";
        return false;
    }
    return true;
}

// --------------------------------------------------------------------------
// media-browser-search
// --------------------------------------------------------------------------

SApplyResult SMediaBrowserSearchAction::apply( SProject * /*project*/ )
{
    SMainWindow *win = mainWindow();
    if( !win ) { qWarning() << "media-browser-search: no main window"; return { false, nullptr }; }
    if( !win->mediaBrowserSearch( needle_, recursive_, debounce_ ) )
        return { false, nullptr };
    // waitMs="0" is "issue and return": three of those back to back is how the
    // supersession AC is written, and the panel drops the stale batches BY ID.
    if( !waitIdle( win, waitMs_ ) ) return { false, nullptr };
    return { true, nullptr };
}

void SMediaBrowserSearchAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "needle", needle_ );
    elem.setAttribute( "recursive", recursive_ ? "1" : "0" );
    elem.setAttribute( "waitMs", waitMs_ );
    elem.setAttribute( "debounce", debounce_ ? "1" : "0" );
}

bool SMediaBrowserSearchAction::readXml( const QDomElement &elem, int )
{
    needle_    = elem.attribute( "needle" );
    recursive_ = elem.attribute( "recursive", "0" ) != "0";
    waitMs_    = elem.attribute( "waitMs", "2000" ).toInt();
    debounce_  = elem.attribute( "debounce", "0" ) != "0";
    return true;
}

// --------------------------------------------------------------------------
// media-browser-filter
// --------------------------------------------------------------------------

SApplyResult SMediaBrowserFilterAction::apply( SProject * /*project*/ )
{
    SMainWindow *win = mainWindow();
    if( !win ) { qWarning() << "media-browser-filter: no main window"; return { false, nullptr }; }
    if( !win->mediaBrowserSetFilter( categories_ ) ) return { false, nullptr };
    if( !waitIdle( win, waitMs_ ) ) return { false, nullptr };
    return { true, nullptr };
}

void SMediaBrowserFilterAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "categories", categories_ );
    elem.setAttribute( "waitMs", waitMs_ );
}

bool SMediaBrowserFilterAction::readXml( const QDomElement &elem, int )
{
    categories_ = elem.attribute( "categories", "audio" );
    waitMs_     = elem.attribute( "waitMs", "2000" ).toInt();
    return true;
}

// --------------------------------------------------------------------------
// media-browser-drag
// --------------------------------------------------------------------------

SApplyResult SMediaBrowserDragAction::apply( SProject * /*project*/ )
{
    SMainWindow *win = mainWindow();
    if( !win ) { qWarning() << "media-browser-drag: no main window"; return { false, nullptr }; }
    if( !win->mediaBrowserDrag( row_, name_, trackPath_, (offset_t) timePos_ ) )
        return { false, nullptr };
    // The drop submits its own SAddSampleAction; undoing THAT is what reverses
    // the gesture, which is why this verb has no inverse of its own.
    return { true, nullptr };
}

void SMediaBrowserDragAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "row", row_ );
    elem.setAttribute( "name", name_ );
    elem.setAttribute( "trackPath", trackPath_ );
    elem.setAttribute( "timePos", QString::number( timePos_ ) );
}

bool SMediaBrowserDragAction::readXml( const QDomElement &elem, int )
{
    row_       = elem.attribute( "row", "-1" ).toInt();
    name_      = elem.attribute( "name" );
    trackPath_ = elem.attribute( "trackPath", "0" );
    timePos_   = elem.attribute( "timePos", "0" ).toLongLong();
    if( row_ < 0 && name_.isEmpty() ) {
        qWarning() << "media-browser-drag: needs a row= or a name=";
        return false;
    }
    return true;
}

// --------------------------------------------------------------------------
// assert-media-browser
// --------------------------------------------------------------------------

SApplyResult SAssertMediaBrowserAction::apply( SProject * /*project*/ )
{
    SMainWindow *win = mainWindow();
    if( !win ) { qWarning() << "assert-media-browser: no main window"; return { false, nullptr }; }
    if( !waitIdle( win, waitMs_ ) ) return { false, nullptr };

    const QString desc = win->describeMediaBrowser();
    if( desc.isEmpty() ) {
        qWarning() << "assert-media-browser FAILED: no media browser panel";
        return { false, nullptr };
    }

    if( !contains_.isEmpty() && !desc.contains( contains_ ) ) {
        qWarning() << "assert-media-browser FAILED: expected" << contains_
                   << "in\n" << desc;
        return { false, nullptr };
    }
    if( !absent_.isEmpty() && desc.contains( absent_ ) ) {
        qWarning() << "assert-media-browser FAILED: unexpected" << absent_
                   << "in\n" << desc;
        return { false, nullptr };
    }
    if( !mode_.isEmpty()
        && !desc.startsWith( QStringLiteral( "mode=" ) + mode_ + '|' ) ) {
        qWarning() << "assert-media-browser FAILED: expected mode" << mode_
                   << "in\n" << desc;
        return { false, nullptr };
    }
    if( rowCount_ >= 0
        && !desc.contains( QStringLiteral( "|rows=%1|" ).arg( rowCount_ ) ) ) {
        qWarning() << "assert-media-browser FAILED: expected" << rowCount_
                   << "rows in\n" << desc;
        return { false, nullptr };
    }
    if( truncated_ >= 0
        && !desc.contains( QStringLiteral( "|truncated=%1|" ).arg( truncated_ ) ) ) {
        qWarning() << "assert-media-browser FAILED: expected truncated="
                   << truncated_ << "in\n" << desc;
        return { false, nullptr };
    }
    return { true, nullptr };
}

void SAssertMediaBrowserAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "contains", contains_ );
    elem.setAttribute( "absent", absent_ );
    elem.setAttribute( "rowCount", rowCount_ );
    elem.setAttribute( "truncated", truncated_ );
    elem.setAttribute( "mode", mode_ );
    elem.setAttribute( "waitMs", waitMs_ );
}

bool SAssertMediaBrowserAction::readXml( const QDomElement &elem, int )
{
    contains_  = elem.attribute( "contains" );
    absent_    = elem.attribute( "absent" );
    rowCount_  = elem.attribute( "rowCount", "-1" ).toInt();
    truncated_ = elem.attribute( "truncated", "-1" ).toInt();
    mode_      = elem.attribute( "mode" );
    waitMs_    = elem.attribute( "waitMs", "0" ).toInt();
    return true;
}

// --------------------------------------------------------------------------
// proposal 38 GATE 5b -- the Nextcloud accounts model and Options -> Media.
// --------------------------------------------------------------------------

namespace {

// A bounded wait for one async SMediaSource request, driven the same way
// SWebDavClient's own tests wait: a local QEventLoop, quit by whichever of
// the three terminal signals fires first, or by a timeout. Never sleeps.
void waitForMediaRequest( SMediaSource *src, int requestId, int timeoutMs )
{
    if( !src || requestId <= 0 ) return;
    QEventLoop loop;
    QTimer     to;
    to.setSingleShot( true );
    QObject::connect( &to, &QTimer::timeout, &loop, &QEventLoop::quit );
    QObject::connect( src, &SMediaSource::entriesReady, &loop,
                      [&]( int id, const QVector<SMediaEntry> &, bool final, int ) {
        if( id == requestId && final ) loop.quit();
    } );
    QObject::connect( src, &SMediaSource::requestFailed, &loop,
                      [&]( int id, const QString & ) {
        if( id == requestId ) loop.quit();
    } );
    to.start( timeoutMs );
    loop.exec();
}

}   // namespace

// --------------------------------------------------------------------------
// set-media-account
// --------------------------------------------------------------------------

SApplyResult SSetMediaAccountAction::apply( SProject * /*project*/ )
{
    SMediaAccountManager *mgr = SApplication::app().mediaAccounts();
    QString error;
    const bool ok = mgr->setAccount( accountId_, url_, user_, password_, remember_, &error );
    if( ok != expectOk_ ) {
        qWarning() << "set-media-account: expected ok=" << expectOk_ << "got" << ok
                   << "(" << error << ")";
        return { false, nullptr };
    }
    if( !ok ) qDebug().noquote() << "set-media-account: refused as expected -" << error;
    return { true, nullptr };   // a test verb has nothing to undo
}

void SSetMediaAccountAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "accountId", accountId_ );
    elem.setAttribute( "url", url_ );
    elem.setAttribute( "user", user_ );
    elem.setAttribute( "password", password_ );
    elem.setAttribute( "remember", remember_ ? "1" : "0" );
    elem.setAttribute( "expectOk", expectOk_ ? "1" : "0" );
}

bool SSetMediaAccountAction::readXml( const QDomElement &elem, int )
{
    accountId_ = elem.attribute( "accountId" );
    url_       = elem.attribute( "url" );
    user_      = elem.attribute( "user" );
    password_  = elem.attribute( "password" );
    remember_  = elem.attribute( "remember", "1" ) != "0";
    expectOk_  = elem.attribute( "expectOk", "1" ) != "0";
    if( accountId_.isEmpty() ) {
        qWarning() << "set-media-account: needs an accountId=";
        return false;
    }
    return true;
}

// --------------------------------------------------------------------------
// remove-media-account
// --------------------------------------------------------------------------

SApplyResult SRemoveMediaAccountAction::apply( SProject * /*project*/ )
{
    SApplication::app().mediaAccounts()->removeAccount( accountId_ );
    return { true, nullptr };
}

void SRemoveMediaAccountAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "accountId", accountId_ );
}

bool SRemoveMediaAccountAction::readXml( const QDomElement &elem, int )
{
    accountId_ = elem.attribute( "accountId" );
    if( accountId_.isEmpty() ) {
        qWarning() << "remove-media-account: needs an accountId=";
        return false;
    }
    return true;
}

// --------------------------------------------------------------------------
// media-test-connection
// --------------------------------------------------------------------------

SApplyResult SMediaTestConnectionAction::apply( SProject * /*project*/ )
{
    SMediaAccountManager *mgr = SApplication::app().mediaAccounts();

    std::unique_ptr<SWebDavStub> stub;
    QString effectiveUrl = url_;
    if( useStub_ == QLatin1String( "ok" ) || useStub_ == QLatin1String( "401" ) ) {
        stub.reset( new SWebDavStub );
        if( !stub->start() ) {
            qWarning() << "media-test-connection: could not start the throwaway stub";
            return { false, nullptr };
        }
        stub->setDirectory( "", { SWebDavStubEntry{ "Music", true, -1, QDateTime(), QString(), {} } } );
        if( useStub_ == QLatin1String( "401" ) ) stub->setFault( "", SWebDavStub::Fault::Status401 );
        effectiveUrl = stub->baseUrl().toString();
    }

    const QString result = mgr->testConnection( effectiveUrl, user_, password_ );
    if( stub ) stub->stop();

    bool ok = true;
    if( !expectContains_.isEmpty() && !result.contains( expectContains_ ) ) {
        qWarning() << "media-test-connection: expected" << expectContains_ << "in" << result;
        ok = false;
    }
    if( !expectAbsent_.isEmpty() && result.contains( expectAbsent_ ) ) {
        qWarning() << "media-test-connection: unexpected" << expectAbsent_ << "in" << result;
        ok = false;
    }
    qDebug().noquote() << "media-test-connection:" << result;
    return { ok, nullptr };
}

void SMediaTestConnectionAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "url", url_ );
    elem.setAttribute( "user", user_ );
    elem.setAttribute( "password", password_ );
    elem.setAttribute( "useStub", useStub_ );
    elem.setAttribute( "expectContains", expectContains_ );
    elem.setAttribute( "expectAbsent", expectAbsent_ );
}

bool SMediaTestConnectionAction::readXml( const QDomElement &elem, int )
{
    url_            = elem.attribute( "url" );
    user_           = elem.attribute( "user" );
    password_       = elem.attribute( "password" );
    useStub_        = elem.attribute( "useStub" );
    expectContains_ = elem.attribute( "expectContains" );
    expectAbsent_   = elem.attribute( "expectAbsent" );
    return true;
}

// --------------------------------------------------------------------------
// media-account-redaction-drive
// --------------------------------------------------------------------------

SApplyResult SMediaAccountRedactionDriveAction::apply( SProject * /*project*/ )
{
    if( password_.isEmpty() ) {
        qWarning() << "media-account-redaction-drive: needs a password= (the literal AC 14 checks for)";
        return { false, nullptr };
    }

    SWebDavStub stub;
    if( !stub.start() ) {
        qWarning() << "media-account-redaction-drive: could not start the throwaway stub";
        return { false, nullptr };
    }
    stub.setDirectory( "", { SWebDavStubEntry{ "Music", true, -1, QDateTime(), QString(), {} } } );
    stub.setFault( "protected", SWebDavStub::Fault::Status401 );

    // The REAL production path: SSettings + SSecretStore, exactly what the
    // dialog's Save button calls.
    SMediaAccountManager *mgr = SApplication::app().mediaAccounts();
    QString error;
    if( !mgr->setAccount( accountId_, stub.baseUrl().toString(), user_, password_, remember_, &error ) ) {
        qWarning() << "media-account-redaction-drive: setAccount failed -" << error;
        stub.stop();
        return { false, nullptr };
    }

    SMediaSource *src = SMediaRegistry::instance().source( QStringLiteral( "nextcloud:" ) + accountId_ );
    if( !src ) {
        qWarning() << "media-account-redaction-drive: account was not registered as a source";
        stub.stop();
        return { false, nullptr };
    }

    // "a browse" -- succeeds.
    waitForMediaRequest( src, src->listDirectory( QString() ), 3000 );
    // "a failing request" -- 401.
    waitForMediaRequest( src, src->listDirectory( QStringLiteral( "protected" ) ), 3000 );

    stub.stop();
    return { true, nullptr };
}

void SMediaAccountRedactionDriveAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "accountId", accountId_ );
    elem.setAttribute( "user", user_ );
    elem.setAttribute( "password", password_ );
    elem.setAttribute( "remember", remember_ ? "1" : "0" );
}

bool SMediaAccountRedactionDriveAction::readXml( const QDomElement &elem, int )
{
    accountId_ = elem.attribute( "accountId", "qxatest" );
    user_      = elem.attribute( "user", "qxauser" );
    password_  = elem.attribute( "password" );
    remember_  = elem.attribute( "remember", "1" ) != "0";
    return true;
}

// --------------------------------------------------------------------------
// assert-media-options
// --------------------------------------------------------------------------

SApplyResult SAssertMediaOptionsAction::apply( SProject * )
{
    // The REAL dialog, built off screen and never shown -- the house pattern
    // (assert-midi-options). Its constructor runs loadMediaPage(), which is
    // the code under test, including the plaintext-migration rescan (AC 17).
    SOptionsDialog dialog( nullptr );
    const QString desc = dialog.describeMediaPage();

    bool ok = true;
    QStringList problems;
    if( !contains_.isEmpty() && !desc.contains( contains_ ) ) {
        ok = false;
        problems << QString( "expected to contain '%1'" ).arg( contains_ );
    }
    if( !absent_.isEmpty() && desc.contains( absent_ ) ) {
        ok = false;
        problems << QString( "expected NOT to contain '%1'" ).arg( absent_ );
    }
    if( accountCount_ >= 0
        && !desc.contains( QString( "accounts=%1\n" ).arg( accountCount_ ) ) ) {
        ok = false;
        problems << QString( "expected accounts=%1" ).arg( accountCount_ );
    }

    if( !ok ) {
        qWarning().noquote() << "assert-media-options: FAILED -" << problems.join( "; " )
                             << "\n" << desc;
        return { false, nullptr };
    }
    qDebug().noquote() << "assert-media-options: OK\n" << desc;
    return { true, nullptr };
}

void SAssertMediaOptionsAction::writeXml( QDomElement &elem ) const
{
    if( !contains_.isEmpty() ) elem.setAttribute( "contains", contains_ );
    if( !absent_.isEmpty() )   elem.setAttribute( "absent", absent_ );
    elem.setAttribute( "accountCount", accountCount_ );
}

bool SAssertMediaOptionsAction::readXml( const QDomElement &elem, int )
{
    contains_     = elem.attribute( "contains" );
    absent_       = elem.attribute( "absent" );
    accountCount_ = elem.attribute( "accountCount", "-1" ).toInt();
    return true;
}

// --------------------------------------------------------------------------
// assert-settings-file
// --------------------------------------------------------------------------

SApplyResult SAssertSettingsFileAction::apply( SProject * )
{
    if( contains_.isEmpty() && absent_.isEmpty() ) {
        qWarning() << "assert-settings-file: needs a contains= or an absent=";
        return { false, nullptr };
    }

    const QString path = SSettings::instance().configDir() + QStringLiteral( "/smaragd.ini" );
    QFile f( path );
    if( !f.open( QIODevice::ReadOnly ) ) {
        qWarning() << "assert-settings-file: cannot open" << path;
        return { false, nullptr };
    }
    const QString content = QString::fromUtf8( f.readAll() );
    f.close();

    bool ok = true;
    if( !contains_.isEmpty() && !content.contains( contains_ ) ) {
        qWarning() << "assert-settings-file:" << path << "does NOT contain" << contains_;
        ok = false;
    }
    if( !absent_.isEmpty() && content.contains( absent_ ) ) {
        qWarning() << "assert-settings-file:" << path << "unexpectedly CONTAINS" << absent_;
        ok = false;
    }
    if( ok ) qDebug() << "assert-settings-file: OK -" << path;
    return { ok, nullptr };
}

void SAssertSettingsFileAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "contains", contains_ );
    elem.setAttribute( "absent", absent_ );
}

bool SAssertSettingsFileAction::readXml( const QDomElement &elem, int )
{
    contains_ = elem.attribute( "contains" );
    absent_   = elem.attribute( "absent" );
    return true;
}

// --------------------------------------------------------------------------

static const bool s_reg_media =
    ( SActionRegistry::instance().registerType(
          QStringLiteral( "media-browser-source" ),
          [] { return new SMediaBrowserSourceAction; } ),
      SActionRegistry::instance().registerType(
          QStringLiteral( "media-browser-path" ),
          [] { return new SMediaBrowserPathAction; } ),
      SActionRegistry::instance().registerType(
          QStringLiteral( "media-browser-search" ),
          [] { return new SMediaBrowserSearchAction; } ),
      SActionRegistry::instance().registerType(
          QStringLiteral( "media-browser-filter" ),
          [] { return new SMediaBrowserFilterAction; } ),
      SActionRegistry::instance().registerType(
          QStringLiteral( "media-browser-drag" ),
          [] { return new SMediaBrowserDragAction; } ),
      SActionRegistry::instance().registerType(
          QStringLiteral( "assert-media-browser" ),
          [] { return new SAssertMediaBrowserAction; } ),
      SActionRegistry::instance().registerType(
          QStringLiteral( "set-media-account" ),
          [] { return new SSetMediaAccountAction; } ),
      SActionRegistry::instance().registerType(
          QStringLiteral( "remove-media-account" ),
          [] { return new SRemoveMediaAccountAction; } ),
      SActionRegistry::instance().registerType(
          QStringLiteral( "media-test-connection" ),
          [] { return new SMediaTestConnectionAction; } ),
      SActionRegistry::instance().registerType(
          QStringLiteral( "media-account-redaction-drive" ),
          [] { return new SMediaAccountRedactionDriveAction; } ),
      SActionRegistry::instance().registerType(
          QStringLiteral( "assert-media-options" ),
          [] { return new SAssertMediaOptionsAction; } ),
      SActionRegistry::instance().registerType(
          QStringLiteral( "assert-settings-file" ),
          [] { return new SAssertSettingsFileAction; } ),
      true );
