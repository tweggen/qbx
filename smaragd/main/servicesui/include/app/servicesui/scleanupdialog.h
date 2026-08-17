#ifndef _SCLEANUPDIALOG_H
#define _SCLEANUPDIALOG_H

#include <QDialog>
#include <QList>
#include <QString>

class QButtonGroup;
class QLabel;

class SProject;

/**
 * The "Cleanup..." dialog behind the resources window's context menu
 * (SExternFileList::cleanupRequested).
 *
 * It offers a radio-button list of cleanup KINDS; today there is exactly one.
 * The list is data (kinds()), not code: adding a second kind is one row there
 * plus one case in runCleanup(), and nothing else in the dialog knows how many
 * kinds exist or what they mean.
 *
 * It lives in app/servicesui rather than next to the list widget because
 * app/model may include no other app module at all (tools/check_layering.py,
 * APP_DEPS['model'] == set()), so the widget can only announce the request.
 */
class SCleanupDialog : public QDialog {
    Q_OBJECT

public:
    enum Kind {
        // Delete recorder-named audio files in the project directory that the
        // project does not reference any more.
        UnusedRecordings = 0,
    };

    struct KindRow {
        Kind kind;
        QString label;
        QString description;
    };

    SCleanupDialog( QWidget *parent, SProject *project );

    // The one place that says what this dialog offers.
    static const QList<KindRow> &kinds();

    void accept() override;

private:
    void updateDescription( int id );
    void runCleanup( Kind kind );
    void cleanupUnusedRecordings();

    SProject *project_;
    QButtonGroup *kindGroup_;
    QLabel *descriptionLabel_;
};

#endif
