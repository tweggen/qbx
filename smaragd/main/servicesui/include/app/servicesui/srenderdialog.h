#ifndef _SRENDERDIALOG_H
#define _SRENDERDIALOG_H

#include "tw/render/render_session.h"

#include <QDialog>
#include <QString>

class QRadioButton;
class QButtonGroup;
class QSpinBox;
class QSlider;
class QLineEdit;
class QPushButton;
class QLabel;

class SProject;

class SRenderDialog : public QDialog {
    Q_OBJECT

public:
    SRenderDialog(SProject *project, QWidget *parent = nullptr);
    ~SRenderDialog() override;

    int exec() override;
    audio::RenderParams getRenderParams() const;

    // The output path as the render will use it, and as Browse... opens on:
    // the line edit's text, absolute and cleaned. A relative entry resolves
    // against the project's directory (Documents when untitled), never the
    // process's working directory (QBX-144). Empty when the line edit is.
    QString resolvedOutputPath() const;

private slots:
    void onFormatChanged(int id);
    void onBrowseClicked();
    void onOutputPathChanged();

private:
    void createFormatGroup();
    void createQualityGroup();
    void createChannelsRow();
    void createExtentGroup();
    void createOutputGroup();
    void updateQualityUI();
    // Whether THIS project can be rendered to MP3; `why` gets the reason if not.
    bool mp3Available(QString *why) const;
    void syncPathExtension();
    QString extensionForSelectedFormat() const;
    QString outputBaseDir() const;
    QString defaultOutputPath() const;
    bool validateInputs();

    SProject *project_;

    // Format selection
    QButtonGroup *formatGroup_ = nullptr;
    QRadioButton *wavRadio_ = nullptr;
    QRadioButton *oggRadio_ = nullptr;
    QRadioButton *mp3Radio_ = nullptr;

    // Quality controls (shown/hidden based on format)
    QSpinBox *wavBitDepthSpinBox_ = nullptr;  // 16, 24, 32
    QSlider *oggQualitySlider_ = nullptr;     // 0-10
    QSpinBox *mp3BitrateSpinBox_ = nullptr;   // 128-320

    // Channel count: a READ-ONLY display of the project's width (proposal 36
    // B8 decision 3), never a control — see createChannelsRow().
    QLabel *channelsLabel_ = nullptr;

    // Extent selection
    QButtonGroup *extentGroup_ = nullptr;
    QRadioButton *entireProjectRadio_ = nullptr;
    QRadioButton *timeSelectionRadio_ = nullptr;

    // Output file path
    QLineEdit *outputPathLineEdit_ = nullptr;
    QPushButton *browseButton_ = nullptr;

    // Buttons
    QPushButton *renderButton_ = nullptr;
    QPushButton *cancelButton_ = nullptr;
};

#endif
