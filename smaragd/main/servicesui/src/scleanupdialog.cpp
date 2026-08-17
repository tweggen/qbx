#include "app/servicesui/scleanupdialog.h"

#include "app/model/sproject.h"
#include "app/model/sexternfile.h"

#include <QAbstractButton>
#include <QButtonGroup>
#include <QDebug>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileInfoList>
#include <QGroupBox>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QRadioButton>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QVBoxLayout>

// A recorded take is named by recording_session.cc:275-281 as
//   <projectDir>/%Y%m%d_%H%M%S_<3-digit-ms>_<trackName>.wav
// so the TIMESTAMP PREFIX is the discriminator, and it is what makes deleting
// by name safe at all: the project file itself, an imported sample and any
// hand-named file cannot match it. The extension set is wider than what the
// recorder writes today (only .wav) purely so a later recorder format is not
// silently left behind; the prefix, not the extension, is the safety property.
static const char *kRecordingPattern =
    "^\\d{8}_\\d{6}_\\d{3}_.*\\.(wav|ogg|flac|aif|aiff|mp3)$";

// Canonical spelling of a path, for comparing a directory entry against a
// project reference so a relative/absolute, separator or symlink difference can
// never make a referenced file look unreferenced. canonicalFilePath() returns
// EMPTY for a path that does not exist — a project may legitimately reference a
// file that has been moved away — so fall back to the absolute spelling rather
// than to nothing, which would compare equal to every other missing reference.
static QString canonicalOf( const QString &path )
{
    QFileInfo fi( path );
    QString canonical = fi.canonicalFilePath();
    return canonical.isEmpty() ? fi.absoluteFilePath() : canonical;
}

const QList<SCleanupDialog::KindRow> &SCleanupDialog::kinds()
{
    static const QList<KindRow> rows = {
        { UnusedRecordings,
          QObject::tr( "Unused recordings in project directory..." ),
          QObject::tr( "Deletes audio files named by the recorder "
                       "(YYYYMMDD_HHMMSS_mmm_<track>) that lie directly in the "
                       "project directory and are not referenced by the project "
                       "any more. Subdirectories are never touched, and every "
                       "file is listed for confirmation before anything is "
                       "deleted." ) },
    };
    return rows;
}

SCleanupDialog::SCleanupDialog( QWidget *parent, SProject *project )
    : QDialog( parent ),
      project_( project ),
      kindGroup_( nullptr ),
      descriptionLabel_( nullptr )
{
    setWindowTitle( tr( "Cleanup" ) );
    setModal( true );

    QVBoxLayout *outer = new QVBoxLayout( this );
    outer->addWidget( new QLabel( tr( "Choose what to clean up:" ), this ) );

    QGroupBox *box = new QGroupBox( this );
    QVBoxLayout *boxLayout = new QVBoxLayout( box );
    kindGroup_ = new QButtonGroup( this );
    const QList<KindRow> &rows = kinds();
    for( const auto &row : rows ) {
        QRadioButton *rb = new QRadioButton( row.label, box );
        // The button id IS the enum value, so checkedId() maps straight back to
        // a Kind and no parallel index has to be kept in step.
        kindGroup_->addButton( rb, (int) row.kind );
        boxLayout->addWidget( rb );
    }
    outer->addWidget( box );

    descriptionLabel_ = new QLabel( this );
    descriptionLabel_->setWordWrap( true );
    descriptionLabel_->setMinimumWidth( 380 );
    outer->addWidget( descriptionLabel_ );

    QObject::connect( kindGroup_, &QButtonGroup::idToggled,
                      this, [this]( int id, bool checked ) {
        if( checked ) updateDescription( id );
    } );

    // Pre-select the first kind — the connect above is already in place, so this
    // also fills the description.
    if( !rows.isEmpty() ) {
        QAbstractButton *first = kindGroup_->button( (int) rows.first().kind );
        if( first ) first->setChecked( true );
    }

    QDialogButtonBox *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this );
    QObject::connect( buttons, &QDialogButtonBox::accepted, this, &SCleanupDialog::accept );
    QObject::connect( buttons, &QDialogButtonBox::rejected, this, &SCleanupDialog::reject );
    outer->addWidget( buttons );
}

void SCleanupDialog::updateDescription( int id )
{
    if( !descriptionLabel_ ) return;
    for( const auto &row : kinds() ) {
        if( (int) row.kind == id ) {
            descriptionLabel_->setText( row.description );
            return;
        }
    }
    descriptionLabel_->clear();
}

void SCleanupDialog::accept()
{
    int id = kindGroup_ ? kindGroup_->checkedId() : -1;
    if( id < 0 ) {
        // No kind selected (only reachable if kinds() is ever empty) — treat OK
        // as a cancel rather than guessing at one.
        QDialog::reject();
        return;
    }
    runCleanup( (Kind) id );
    QDialog::accept();
}

void SCleanupDialog::runCleanup( Kind kind )
{
    switch( kind ) {
    case UnusedRecordings:
        cleanupUnusedRecordings();
        break;
    }
}

void SCleanupDialog::cleanupUnusedRecordings()
{
    const QString projectFile = project_ ? project_->projectFilePath() : QString();
    if( projectFile.isEmpty() ) {
        // The recorder writes next to the project FILE (smainwindow.cpp ~812),
        // so without one there is no directory this cleanup could mean.
        QMessageBox::information( this, tr( "Cleanup" ),
            tr( "This project has not been saved yet, so it has no project "
                "directory.\n\nRecordings are written next to the project file; "
                "save the project first, then run the cleanup again." ) );
        return;
    }

    const QString dirPath = QFileInfo( projectFile ).absolutePath();
    QDir dir( dirPath );

    // What the project references RIGHT NOW. Dict presence — not a zero
    // ref-count — is deliberately the test: SObject::removeRef() drops an
    // unreferenced body through deleteLater(), and ~SPlainWave is what
    // deregisters it from SProject's externFileDict_, so a body sitting in the
    // dict at zero references is exactly the one an UNDO is about to re-attach.
    // Deleting its file would break that undo. This dialog runs its own event
    // loop, so any deferred delete that was already pending has settled by the
    // time we get here and the dict is current.
    QSet<QString> referenced;
    if( project_ ) {
        const QHash<QString,SExternFile*> &files = project_->externFiles();
        for( auto it = files.constBegin(); it != files.constEnd(); ++it ) {
            // The key IS the file name the project registered; getFileName() is
            // the same string for a live body. Take both, so a body that is mid
            // teardown cannot open a hole.
            referenced.insert( canonicalOf( it.key() ) );
            if( it.value() ) referenced.insert( canonicalOf( it.value()->getFileName() ) );
        }
    }

    const QRegularExpression recordingRe( QString::fromLatin1( kRecordingPattern ),
                                          QRegularExpression::CaseInsensitiveOption );

    // NON-recursive, files only. QDir::NoSymLinks drops a symlink from the
    // listing entirely rather than resolving it, so a link in the project
    // directory can never make us delete a file somewhere else; QDir::Files
    // excludes directories, which are never candidates.
    const QFileInfoList entries = dir.entryInfoList(
        QDir::Files | QDir::NoSymLinks | QDir::NoDotAndDotDot, QDir::Name );

    QStringList candidateNames;
    QStringList candidatePaths;
    qint64 totalBytes = 0;
    for( const QFileInfo &fi : entries ) {
        if( !recordingRe.match( fi.fileName() ).hasMatch() ) continue;
        if( referenced.contains( canonicalOf( fi.absoluteFilePath() ) ) ) continue;
        candidateNames << fi.fileName();
        candidatePaths << fi.absoluteFilePath();
        totalBytes += fi.size();
    }

    if( candidatePaths.isEmpty() ) {
        QMessageBox::information( this, tr( "Cleanup" ),
            tr( "No unused recordings found in %1." ).arg( QDir::toNativeSeparators( dirPath ) ) );
        return;
    }

    QMessageBox confirm( this );
    confirm.setIcon( QMessageBox::Warning );
    confirm.setWindowTitle( tr( "Cleanup" ) );
    confirm.setText( tr( "Permanently delete %n unused recording(s) (%1) from %2?",
                         "", candidatePaths.size() )
                         .arg( QLocale().formattedDataSize( totalBytes ),
                               QDir::toNativeSeparators( dirPath ) ) );
    confirm.setInformativeText( tr( "This cannot be undone." ) );
    confirm.setDetailedText( candidateNames.join( QLatin1Char( '\n' ) ) );
    confirm.setStandardButtons( QMessageBox::Yes | QMessageBox::Cancel );
    // Deletion is irreversible: the safe answer is the default one.
    confirm.setDefaultButton( QMessageBox::Cancel );
    if( confirm.exec() != QMessageBox::Yes ) return;

    int deleted = 0;
    QStringList failed;
    for( const QString &path : candidatePaths ) {
        if( QFile::remove( path ) ) {
            ++deleted;
        } else {
            failed << QFileInfo( path ).fileName();
            qWarning() << "SCleanupDialog: could not delete" << path;
        }
    }

    if( failed.isEmpty() ) {
        QMessageBox::information( this, tr( "Cleanup" ),
            tr( "Deleted %n unused recording(s).", "", deleted ) );
    } else {
        QMessageBox::warning( this, tr( "Cleanup" ),
            tr( "Deleted %1 of %2 recordings.\n\nCould not delete:\n%3" )
                .arg( deleted )
                .arg( candidatePaths.size() )
                .arg( failed.join( QLatin1Char( '\n' ) ) ) );
    }
}
