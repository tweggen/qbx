#ifndef SOPTIONSDIALOG_H
#define SOPTIONSDIALOG_H

#include <QDialog>

class QTreeWidget;
class QStackedWidget;
class QComboBox;
class QCheckBox;

// Per-user preferences dialog: a category tree on the left, one option page per
// category on the right (QStackedWidget), with OK / Cancel / Apply. Reads from
// and writes to SSettings. Categories: Mouse navigation, Audio.
class SOptionsDialog : public QDialog
{
    Q_OBJECT
public:
    explicit SOptionsDialog( QWidget *parent = nullptr );

private slots:
    void apply();             // write all pages to SSettings (no close)
    void accept() override;   // apply + close

private:
    QWidget *buildMousePage();
    QWidget *buildAudioPage();
    void loadMousePage();
    void loadAudioPage();
    void applyMousePage();
    void applyAudioPage();

    QTreeWidget    *tree_;
    QStackedWidget *stack_;

    // Mouse-navigation page.
    QComboBox *wheelPlain_;
    QComboBox *wheelShift_;
    QComboBox *wheelCtrl_;
    QComboBox *wheelCtrlShift_;
    QCheckBox *zoomToCursor_;
    QCheckBox *invertZoom_;

    // Audio page.
    QComboBox *audioDevice_;      // Output device
    QComboBox *audioInputDevice_; // Input device (for recording)
};

#endif // SOPTIONSDIALOG_H
