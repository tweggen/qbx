#include "app/testkit/smediatestactions.h"

#include "app/actions/sactionregistry.h"
#include "app/media/smediaregistry.h"
#include "app/media/smediasource.h"
#include "app/shell/sapplication.h"
#include "app/media/sdelayedlocalsource.h"
#include "app/media/smediacache.h"
#include "app/media/smediadrop.h"
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
#include <QDir>
#include <QFileInfo>
#include <QPointer>
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

    if( !activate_.isEmpty() ) {
        if( !win->mediaBrowserActivate( activate_ ) ) {
            qWarning() << "media-browser-path: no row named" << activate_;
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
    elem.setAttribute( "activate", activate_ );
    elem.setAttribute( "waitMs", waitMs_ );
}

bool SMediaBrowserPathAction::readXml( const QDomElement &elem, int )
{
    path_     = elem.attribute( "path" );
    expand_   = elem.attribute( "expand" );
    activate_ = elem.attribute( "activate" );
    waitMs_   = elem.attribute( "waitMs", "2000" ).toInt();
    if( path_.isEmpty() && expand_.isEmpty() && activate_.isEmpty() ) {
        qWarning() << "media-browser-path: needs a path=, an expand= or an activate=";
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
    if( !win->mediaBrowserDrag( row_, name_, trackPath_, (offset_t) timePos_,
                               belowLastTrack_ ) )
        return { false, nullptr };
    // The drop submits its own SAddSampleAction (and, for belowLastTrack, an
    // add-track ahead of it, as a SEPARATE undo step — see
    // SStdMixerView::ctAddTrackBelowLast); undoing THOSE is what reverses the
    // gesture, which is why this verb has no inverse of its own.
    return { true, nullptr };
}

void SMediaBrowserDragAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "row", row_ );
    elem.setAttribute( "name", name_ );
    elem.setAttribute( "trackPath", trackPath_ );
    elem.setAttribute( "timePos", QString::number( timePos_ ) );
    if( belowLastTrack_ ) elem.setAttribute( "belowLastTrack", "1" );
}

bool SMediaBrowserDragAction::readXml( const QDomElement &elem, int )
{
    row_            = elem.attribute( "row", "-1" ).toInt();
    name_           = elem.attribute( "name" );
    trackPath_      = elem.attribute( "trackPath", "0" );
    timePos_        = elem.attribute( "timePos", "0" ).toLongLong();
    belowLastTrack_ = elem.attribute( "belowLastTrack", "0" ).toInt() != 0;
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
// media-test-source (proposal 38 gate 3)
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

SDelayedLocalSource *delayedSource()
{
    return qobject_cast<SDelayedLocalSource *>(
        SMediaRegistry::instance().source(
            SDelayedLocalSource::sourceIdString() ) );
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
// media-test-source (proposal 38 gate 3)
// --------------------------------------------------------------------------

SApplyResult SMediaTestSourceAction::apply( SProject * /*project*/ )
{
    SDelayedLocalSource *src = delayedSource();
    if( !src ) {
        // REFUSED, never a silent no-op: the source exists only under
        // SMARAGD_MEDIA_TEST_SOURCE=1, and a case that configured nothing and
        // carried on would then be measuring the wrong provider.
        qWarning() << "media-test-source: no 'testdelay' source is registered "
                      "(SMARAGD_MEDIA_TEST_SOURCE=1?)";
        return { false, nullptr };
    }
    if( delayMs_ >= 0 )  src->setFetchDelayMs( delayMs_ );
    if( hasFailPath_ )   src->setFailSubstring( failPath_ );
    if( reset_ ) {
        src->resetFetchCount();
        smediadrop::resetCounters();
    }
    if( clearCache_ && !SMediaCache::instance().clearAll() ) {
        qWarning() << "media-test-source: the cache refused to clear "
                      "(SMARAGD_MEDIA_CACHE_DIR unset?)";
        return { false, nullptr };
    }
    return { true, nullptr };
}

void SMediaTestSourceAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "delayMs", delayMs_ );
    elem.setAttribute( "failPath", failPath_ );
    elem.setAttribute( "reset", reset_ ? "1" : "0" );
    elem.setAttribute( "clearCache", clearCache_ ? "1" : "0" );
}

bool SMediaTestSourceAction::readXml( const QDomElement &elem, int )
{
    delayMs_     = elem.attribute( "delayMs", "-1" ).toInt();
    hasFailPath_ = elem.hasAttribute( "failPath" );
    failPath_    = elem.attribute( "failPath" );
    reset_       = elem.attribute( "reset", "0" ) == QLatin1String( "1" );
    clearCache_  = elem.attribute( "clearCache", "0" ) == QLatin1String( "1" );
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

    QString effectiveUser = user_;
    if( useAccountUrl_ ) {
        const SMediaAccountInfo info = mgr->account( accountId_ );
        effectiveUrl  = info.url;
        effectiveUser = info.user;
    }

    const QString result =
        accountId_.isEmpty()
            ? mgr->testConnection( effectiveUrl, effectiveUser, password_ )
            : mgr->testAccountConnection( accountId_, effectiveUrl, effectiveUser, password_ );
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
    elem.setAttribute( "accountId", accountId_ );
    elem.setAttribute( "useAccountUrl", useAccountUrl_ ? "1" : "0" );
    elem.setAttribute( "expectContains", expectContains_ );
    elem.setAttribute( "expectAbsent", expectAbsent_ );
}

bool SMediaTestConnectionAction::readXml( const QDomElement &elem, int )
{
    url_            = elem.attribute( "url" );
    user_           = elem.attribute( "user" );
    password_       = elem.attribute( "password" );
    useStub_        = elem.attribute( "useStub" );
    accountId_      = elem.attribute( "accountId" );
    useAccountUrl_  = elem.attribute( "useAccountUrl", "0" ) != QLatin1String( "0" );
    expectContains_ = elem.attribute( "expectContains" );
    expectAbsent_   = elem.attribute( "expectAbsent" );

    // REFUSED at parse time, never silently resolved: both attributes decide
    // which URL is tested, so a case naming both is a case whose author does
    // not know which server answered.
    if( useAccountUrl_ && !useStub_.isEmpty() ) {
        qWarning() << "media-test-connection: useAccountUrl= and useStub= both pick "
                      "the URL - name one";
        return false;
    }
    if( useAccountUrl_ && accountId_.isEmpty() ) {
        qWarning() << "media-test-connection: useAccountUrl= needs an accountId=";
        return false;
    }
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
    // initialPage_ == -2 (the attribute was never given) maps to -1, the
    // ctor's own "no explicit page" sentinel, so a case that never mentions
    // initialPage builds exactly as every case before AC-b1 did -- which now
    // means "open on the remembered page" rather than always page 0. Passing
    // a real index is what a case uses to assert that an EXPLICIT page still
    // wins over whatever is remembered.
    SOptionsDialog dialog( nullptr, initialPage_ == -2 ? -1 : initialPage_ );
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
    if( initialPage_ != -2 ) elem.setAttribute( "initialPage", initialPage_ );
}

bool SAssertMediaOptionsAction::readXml( const QDomElement &elem, int )
{
    contains_     = elem.attribute( "contains" );
    absent_       = elem.attribute( "absent" );
    accountCount_ = elem.attribute( "accountCount", "-1" ).toInt();
    initialPage_  = elem.attribute( "initialPage", "-2" ).toInt();
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
// media-drop-wait (proposal 38 gate 3)
// --------------------------------------------------------------------------

SApplyResult SMediaDropWaitAction::apply( SProject * /*project*/ )
{
    QElapsedTimer t;
    t.start();
    while( smediadrop::pendingCount() > 0 ) {
        QCoreApplication::processEvents();
        if( t.elapsed() > waitMs_ ) {
            qWarning() << "media-drop-wait:" << smediadrop::pendingCount()
                       << "placement(s) still pending after" << waitMs_ << "ms";
            return { false, nullptr };
        }
        QThread::msleep( 2 );
    }
    // One more turn: fetchFinished is delivered queued, and the LAST completion
    // clears the pending list before the placement it triggers has been
    // processed by anything downstream.
    QCoreApplication::processEvents();

    struct Check { const char *what; int want; int got; };
    const Check checks[] = {
        { "placed",    placed_,    smediadrop::placedCount() },
        { "failed",    failed_,    smediadrop::failedCount() },
        { "abandoned", abandoned_, smediadrop::abandonedCount() },
    };
    for( const Check &c : checks ) {
        if( c.want >= 0 && c.want != c.got ) {
            qWarning() << "media-drop-wait:" << c.what << "expected" << c.want
                       << "but got" << c.got;
            return { false, nullptr };
        }
    }
    if( fetches_ >= 0 ) {
        SDelayedLocalSource *src = delayedSource();
        const int got = src ? src->fetchCount() : -1;
        if( got != fetches_ ) {
            qWarning() << "media-drop-wait: fetches expected" << fetches_
                       << "but got" << got;
            return { false, nullptr };
        }
    }
    return { true, nullptr };
}

void SMediaDropWaitAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "waitMs", waitMs_ );
    elem.setAttribute( "placed", placed_ );
    elem.setAttribute( "failed", failed_ );
    elem.setAttribute( "abandoned", abandoned_ );
    elem.setAttribute( "fetches", fetches_ );
}

bool SMediaDropWaitAction::readXml( const QDomElement &elem, int )
{
    waitMs_    = elem.attribute( "waitMs", "8000" ).toInt();
    placed_    = elem.attribute( "placed", "-1" ).toInt();
    failed_    = elem.attribute( "failed", "-1" ).toInt();
    abandoned_ = elem.attribute( "abandoned", "-1" ).toInt();
    fetches_   = elem.attribute( "fetches", "-1" ).toInt();
    return true;
}

// --------------------------------------------------------------------------
// media-webdav-stub (proposal 38 GATE 5c)
// --------------------------------------------------------------------------

namespace {

// ONE stub per process. Parented to qApp, so it cannot outlive the application
// object: a QTcpServer torn down after QCoreApplication has gone is a teardown
// crash with a network-shaped delay on it, and the same instinct that aborts
// every QNetworkReply in the source's destructor (§B.7) applies to the other
// end of the wire. A QPointer, not a raw one, so the qApp deletion is
// observable here rather than dangling.
QPointer<SWebDavStub> g_stub;

constexpr int    kMaxMirrorDepth = 8;
constexpr qint64 kMaxMirrorFile  = 32ll * 1024 * 1024;
constexpr qint64 kMaxMirrorTotal = 128ll * 1024 * 1024;

// Mirror a LOCAL directory into the stub's PROPFIND table, recursively, with
// the real file bytes as each entry's GET body -- which is what makes a drop
// out of the browser end in audio a render can be asserted against (AC 19),
// rather than in a placeholder.
//
// The stub's own setDirectory() registers both halves from one call (the
// listing AND every non-dir entry's GET body), so this walk is one call per
// directory and nothing else.
bool mirrorDirectory( SWebDavStub *stub, const QString &absRoot,
                      const QString &relPath, int depth,
                      int &nFiles, qint64 &nBytes, QString *error )
{
    if( depth > kMaxMirrorDepth ) {
        *error = QStringLiteral( "fixture tree deeper than %1 levels at '%2'" )
                     .arg( kMaxMirrorDepth ).arg( relPath );
        return false;
    }
    const QString absHere = relPath.isEmpty()
                                ? absRoot
                                : absRoot + QLatin1Char( '/' ) + relPath;
    QDir dir( absHere );
    if( !dir.exists() ) {
        *error = QStringLiteral( "no such fixture directory: %1" ).arg( absHere );
        return false;
    }

    QVector<SWebDavStubEntry> entries;
    QStringList               subdirs;
    const QFileInfoList       infos =
        dir.entryInfoList( QDir::AllEntries | QDir::NoDotAndDotDot, QDir::Name );
    for( const QFileInfo &fi : infos ) {
        SWebDavStubEntry e;
        e.name  = fi.fileName();
        e.isDir = fi.isDir();
        if( e.isDir ) {
            // A directory's size is the documented -1 ("unknown"), never
            // QFileInfo::size()'s platform junk -- the same rule
            // SLocalMediaSource follows, so the two providers describe() one
            // tree identically and a case can be read against either.
            e.sizeBytes = -1;
            subdirs << ( relPath.isEmpty()
                             ? e.name
                             : relPath + QLatin1Char( '/' ) + e.name );
        } else {
            if( fi.size() > kMaxMirrorFile ) {
                *error = QStringLiteral( "fixture file '%1' is %2 bytes (max %3)" )
                             .arg( fi.fileName() )
                             .arg( fi.size() )
                             .arg( kMaxMirrorFile );
                return false;
            }
            QFile f( fi.absoluteFilePath() );
            if( !f.open( QIODevice::ReadOnly ) ) {
                *error = QStringLiteral( "cannot read fixture file %1" )
                             .arg( fi.absoluteFilePath() );
                return false;
            }
            e.content   = f.readAll();
            e.sizeBytes = e.content.size();
            e.modified  = fi.lastModified().toUTC();
            // An etag the cache's content key can key on, derived from the two
            // things that change when the bytes do.
            e.etag = QStringLiteral( "\"%1-%2\"" )
                         .arg( e.sizeBytes )
                         .arg( fi.lastModified().toUTC().toMSecsSinceEpoch() );
            nBytes += e.sizeBytes;
            ++nFiles;
            if( nBytes > kMaxMirrorTotal ) {
                *error = QStringLiteral( "fixture tree exceeds %1 bytes" )
                             .arg( kMaxMirrorTotal );
                return false;
            }
        }
        entries.append( e );
    }
    stub->setDirectory( relPath, entries );

    for( const QString &sub : subdirs )
        if( !mirrorDirectory( stub, absRoot, sub, depth + 1, nFiles, nBytes, error ) )
            return false;
    return true;
}

bool faultFromWord( const QString &word, SWebDavStub::Fault *out )
{
    if( word.isEmpty() )                          { *out = SWebDavStub::Fault::None;         return true; }
    if( word == QLatin1String( "401" ) )          { *out = SWebDavStub::Fault::Status401;    return true; }
    if( word == QLatin1String( "404" ) )          { *out = SWebDavStub::Fault::Status404;    return true; }
    if( word == QLatin1String( "500" ) )          { *out = SWebDavStub::Fault::Status500;    return true; }
    if( word == QLatin1String( "closemidbody" ) ) { *out = SWebDavStub::Fault::CloseMidBody; return true; }
    if( word == QLatin1String( "stall" ) )        { *out = SWebDavStub::Fault::Stall;        return true; }
    return false;
}

}   // namespace

SApplyResult SMediaWebDavStubAction::apply( SProject * /*project*/ )
{
    if( action_ == QLatin1String( "stop" ) ) {
        // Idempotent on purpose, so a defensive stop costs a case nothing.
        if( g_stub ) {
            g_stub->stop();
            delete g_stub.data();
            g_stub = nullptr;
        }
        return { true, nullptr };
    }

    if( !g_stub ) {
        g_stub = new SWebDavStub( qApp );
        if( !g_stub->start() ) {
            qWarning() << "media-webdav-stub: could not bind 127.0.0.1:0";
            delete g_stub.data();
            g_stub = nullptr;
            return { false, nullptr };
        }
    }

    if( !fixtureDir_.isEmpty() ) {
        const QString absRoot =
            QDir::cleanPath( QDir::current().absoluteFilePath( fixtureDir_ ) );
        g_stub->clearDirectories();
        int     nFiles = 0;
        qint64  nBytes = 0;
        QString error;
        if( !mirrorDirectory( g_stub, absRoot, QString(), 0, nFiles, nBytes, &error ) ) {
            qWarning() << "media-webdav-stub:" << error;
            return { false, nullptr };
        }
        qDebug().noquote()
            << QStringLiteral( "media-webdav-stub: serving %1 file(s) / %2 bytes "
                               "from %3 at %4" )
                   .arg( nFiles )
                   .arg( nBytes )
                   .arg( absRoot, g_stub->baseUrl().toString() );
    }

    // An EMPTY fault= clears every injected fault, so one line restores the
    // healthy server after a failure phase.
    SWebDavStub::Fault fault = SWebDavStub::Fault::None;
    if( !faultFromWord( fault_, &fault ) ) {
        qWarning() << "media-webdav-stub: unknown fault" << fault_;
        return { false, nullptr };
    }
    g_stub->clearFaults();
    if( fault != SWebDavStub::Fault::None ) g_stub->setFault( faultPath_, fault );

    // The credential the server demands. Set unconditionally (empty = do not
    // inspect the header), so one invocation states the whole of the server's
    // state -- the same rule `fault=` follows, and the reason neither is
    // sticky.
    g_stub->setExpectedAuthorization( expectAuth_ );

    if( !accountId_.isEmpty() ) {
        // THE PORT IS WRITTEN WHERE THE FLOW READS IT (see the header): a .qxa
        // cannot interpolate a run-time value into a later attribute, so the
        // verb hands the stub's own base URL straight to the accounts model --
        // the SAME SMediaAccountManager::setAccount() the Options dialog's Save
        // button calls. From here the case names the SOURCE ID, which is
        // static.
        SMediaAccountManager *mgr = SApplication::app().mediaAccounts();
        QString               error;
        if( !mgr->setAccount( accountId_, g_stub->baseUrl().toString(), user_,
                              password_, remember_, &error ) ) {
            qWarning() << "media-webdav-stub: setAccount failed -" << error;
            return { false, nullptr };
        }
        if( !SMediaRegistry::instance().source( QStringLiteral( "nextcloud:" )
                                                + accountId_ ) ) {
            qWarning() << "media-webdav-stub: the account was not registered as a source";
            return { false, nullptr };
        }
    }
    return { true, nullptr };   // a test verb has nothing to undo
}

void SMediaWebDavStubAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "action", action_ );
    elem.setAttribute( "fixtureDir", fixtureDir_ );
    elem.setAttribute( "accountId", accountId_ );
    elem.setAttribute( "user", user_ );
    elem.setAttribute( "password", password_ );
    elem.setAttribute( "remember", remember_ ? "1" : "0" );
    elem.setAttribute( "fault", fault_ );
    elem.setAttribute( "faultPath", faultPath_ );
    elem.setAttribute( "expectAuth", expectAuth_ );
}

bool SMediaWebDavStubAction::readXml( const QDomElement &elem, int )
{
    action_     = elem.attribute( "action", "start" );
    fixtureDir_ = elem.attribute( "fixtureDir" );
    accountId_  = elem.attribute( "accountId" );
    user_       = elem.attribute( "user", "qxauser" );
    password_   = elem.attribute( "password", "qxapass" );
    remember_   = elem.attribute( "remember", "0" ) != "0";
    fault_      = elem.attribute( "fault" );
    faultPath_  = elem.attribute( "faultPath" );
    expectAuth_ = elem.attribute( "expectAuth" );
    if( action_ != QLatin1String( "start" ) && action_ != QLatin1String( "stop" ) ) {
        qWarning() << "media-webdav-stub: action= must be start or stop, not" << action_;
        return false;
    }
    SWebDavStub::Fault ignored;
    if( !faultFromWord( fault_, &ignored ) ) {
        // REFUSED at parse time, never at the first request: a case that
        // misspelled a fault would otherwise assert a healthy server's answer
        // and pass while gating nothing.
        qWarning() << "media-webdav-stub: unknown fault" << fault_;
        return false;
    }
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
      SActionRegistry::instance().registerType(
          QStringLiteral( "media-test-source" ),
          [] { return new SMediaTestSourceAction; } ),
      SActionRegistry::instance().registerType(
          QStringLiteral( "media-drop-wait" ),
          [] { return new SMediaDropWaitAction; } ),
      SActionRegistry::instance().registerType(
          QStringLiteral( "media-webdav-stub" ),
          [] { return new SMediaWebDavStubAction; } ),
      true );
