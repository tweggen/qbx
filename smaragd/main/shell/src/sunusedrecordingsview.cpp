#include "app/shell/sunusedrecordingsview.h"

#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

#include "app/model/sobjectpath.h"
#include "app/model/sproject.h"
#include "app/model/srecordingfiles.h"
#include "app/shell/sapplication.h"
#include "app/shell/saudiorecorder.h"

SUnusedRecordingsView::SUnusedRecordingsView( QWidget *parent )
    : QWidget( parent )
{
    QVBoxLayout *root = new QVBoxLayout( this );

    summary_ = new QLabel( this );
    summary_->setObjectName( QStringLiteral( "unusedRecordingsSummary" ) );
    summary_->setWordWrap( true );
    root->addWidget( summary_ );

    list_ = new QListWidget( this );
    list_->setObjectName( QStringLiteral( "unusedRecordingsList" ) );
    list_->setSelectionMode( QAbstractItemView::NoSelection );
    // READ-ONLY (see the header): the list is §6.3's scan without the removal,
    // and the removal is `cleanup-takes`. Nothing here acts on a row.
    list_->setToolTip(
        tr( "Recordings this project does not play. The audio is still on "
            "disk; nothing here is deleted. Use Clean up takes to remove "
            "them." ) );
    root->addWidget( list_, 1 );

    refresh_ = new QPushButton( tr( "Refresh" ), this );
    refresh_->setObjectName( QStringLiteral( "unusedRecordingsRefresh" ) );
    QHBoxLayout *row = new QHBoxLayout;
    row->addStretch( 1 );
    row->addWidget( refresh_ );
    root->addLayout( row );
    connect( refresh_, &QPushButton::clicked,
             this, &SUnusedRecordingsView::refresh );

    refresh();
}

void SUnusedRecordingsView::refresh()
{
    if( !list_ ) return;
    list_->clear();

    // THE SCAN IS ASKED FRESH, every time. It is a question about the project
    // as it stands -- an undo that puts a take back takes its file off this
    // list -- so a cached answer would be wrong as soon as the user edited.
    SProject *project = SApplication::app().getCurrentProject();
    const QList<srecfiles::Orphan> orphans = srecfiles::scanUnused( project );

    for( const srecfiles::Orphan &o : orphans ) {
        const QString shown = o.path.isEmpty()
                                  ? tr( "(unnamed)" )
                                  : QFileInfo( o.path ).fileName();
        list_->addItem( QStringLiteral( "%1 — %2" ).arg( shown ).arg( o.why ) );
    }
    // The session's losses come after the scan, because they are the ones the
    // user is most likely to be looking for right now.
    for( const QString &s : sessionSkipped_ ) list_->addItem( s );

    const int n = (int) orphans.size() + (int) sessionSkipped_.size();
    summary_->setText(
        n == 0 ? tr( "Nothing unused. Every recording this project holds is "
                     "played by a clip." )
               : tr( "%n recording(s) this project does not play. The audio is "
                     "still on disk.", "", n ) );
}

void SUnusedRecordingsView::onRecordingLanesSkipped( int count,
                                                     const QString &firstWhy )
{
    // THE SIGNAL'S ARGUMENTS ARE NOT ENOUGH, and that is not a flaw in the
    // signal: a status-bar line wants a count and one reason, while this dock
    // wants every lane with its own. So the full list comes off the recorder
    // at the moment it is reported -- which is also the only moment it is
    // valid, because `skippedLanes()` is reset at the start of every pass.
    SAudioRecorder *rec = SApplication::app().audioRecorder();
    if( !rec ) {
        if( count > 0 )
            sessionSkipped_.append(
                tr( "%n track(s) did not take a recording — %1", "", count )
                    .arg( firstWhy ) );
        refresh();
        return;
    }
    for( const SAudioRecorder::SkippedLane &sl : rec->skippedLanes() ) {
        const QString file = sl.file.isEmpty()
                                 ? tr( "(no file)" )
                                 : QFileInfo( sl.file ).fileName();
        sessionSkipped_.append(
            QStringLiteral( "%1 — %2 (%3)" )
                .arg( file )
                .arg( sl.why )
                .arg( strackpath::pathToString( sl.path ) ) );
    }
    refresh();
}
