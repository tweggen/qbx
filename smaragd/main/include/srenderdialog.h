#ifndef _SRENDERDIALOG_H
#define _SRENDERDIALOG_H

#include <audio/audio_file_writer.h>

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

    audio::RenderParams getRenderParams() const;

private slots:
    void onFormatChanged(int id);
    void onBrowseClicked();
    void onOutputPathChanged();

private:
    void createFormatGroup();
    void createQualityGroup();
    void createExtentGroup();
    void createOutputGroup();
    void updateQualityUI();
    bool validateInputs();

    SProject *project_;

    // Format selection
    QButtonGroup *formatGroup_ = nullptr;
    QRadioButton *wavRadio_ = nullptr;
    QRadioButton *oggRadio_ = nullptr;
    QRadioButton *mp3Radio_ = nullptr;

    // Quality controls (shown/hidden based on format)
    QLabel *qualityLabel_ = nullptr;
    QSpinBox *wavBitDepthSpinBox_ = nullptr;  // 16, 24, 32
    QSlider *oggQualitySlider_ = nullptr;     // 0-10
    QSpinBox *mp3BitrateSpinBox_ = nullptr;   // 128-320

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
