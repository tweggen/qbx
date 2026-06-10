#include "srenderdialog.h"

// Forward declare MP3Writer for checking availability
namespace audio {
class MP3Writer {
public:
    static bool isAvailable();
};
}

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QRadioButton>
#include <QButtonGroup>
#include <QSpinBox>
#include <QSlider>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QFileDialog>
#include <QMessageBox>

#include "sproject.h"

SRenderDialog::SRenderDialog(SProject *project, QWidget *parent)
    : QDialog(parent), project_(project) {
    setWindowTitle("Render Project");
    setMinimumWidth(500);

    // Create main layout
    QVBoxLayout *mainLayout = new QVBoxLayout(this);

    // Format group
    createFormatGroup();
    mainLayout->addWidget(new QLabel("Format:"));
    QHBoxLayout *formatLayout = new QHBoxLayout();
    formatLayout->addWidget(wavRadio_);
    formatLayout->addWidget(oggRadio_);
    formatLayout->addWidget(mp3Radio_);
    formatLayout->addStretch();
    mainLayout->addLayout(formatLayout);

    mainLayout->addSpacing(12);

    // Quality group
    createQualityGroup();
    mainLayout->addWidget(new QLabel("Quality:"));

    QVBoxLayout *qualityLayout = new QVBoxLayout();
    qualityLayout->addWidget(new QLabel("WAV bit depth:"));
    qualityLayout->addWidget(wavBitDepthSpinBox_);
    qualityLayout->addWidget(new QLabel("OGG quality (0=low, 10=high):"));
    qualityLayout->addWidget(oggQualitySlider_);
    qualityLayout->addWidget(new QLabel("MP3 bitrate:"));
    qualityLayout->addWidget(mp3BitrateSpinBox_);
    qualityLayout->addStretch();
    mainLayout->addLayout(qualityLayout);
    mainLayout->addSpacing(8);

    mainLayout->addSpacing(12);

    // Extent group
    createExtentGroup();
    mainLayout->addWidget(new QLabel("Render extent:"));
    QVBoxLayout *extentLayout = new QVBoxLayout();
    extentLayout->addWidget(entireProjectRadio_);
    extentLayout->addWidget(timeSelectionRadio_);
    mainLayout->addLayout(extentLayout);

    mainLayout->addSpacing(12);

    // Output file
    createOutputGroup();
    mainLayout->addWidget(new QLabel("Output file:"));
    QHBoxLayout *pathLayout = new QHBoxLayout();
    pathLayout->addWidget(outputPathLineEdit_);
    pathLayout->addWidget(browseButton_);
    mainLayout->addLayout(pathLayout);

    mainLayout->addSpacing(12);

    // Buttons
    QHBoxLayout *buttonLayout = new QHBoxLayout();
    buttonLayout->addStretch();
    buttonLayout->addWidget(renderButton_);
    buttonLayout->addWidget(cancelButton_);
    mainLayout->addLayout(buttonLayout);

    setLayout(mainLayout);

    // Connect signals
    connect(formatGroup_, QOverload<QAbstractButton *>::of(&QButtonGroup::buttonClicked), this,
            [this](QAbstractButton *) { this->onFormatChanged(0); });
    connect(browseButton_, &QPushButton::clicked, this, &SRenderDialog::onBrowseClicked);
    connect(outputPathLineEdit_, &QLineEdit::textChanged, this,
            &SRenderDialog::onOutputPathChanged);
    connect(renderButton_, &QPushButton::clicked, this, &SRenderDialog::accept);
    connect(cancelButton_, &QPushButton::clicked, this, &SRenderDialog::reject);

    // Set defaults
    wavRadio_->setChecked(true);
    entireProjectRadio_->setChecked(true);
    onFormatChanged(0);

    // Time selection is available if project exists
    // (user can set markers at any time)
    if (!project) {
        timeSelectionRadio_->setEnabled(false);
    }
}

SRenderDialog::~SRenderDialog() {}

void SRenderDialog::createFormatGroup() {
    formatGroup_ = new QButtonGroup(this);

    wavRadio_ = new QRadioButton("WAV (PCM 16-bit)");
    oggRadio_ = new QRadioButton("OGG Vorbis");
    mp3Radio_ = new QRadioButton("MP3");

    formatGroup_->addButton(wavRadio_, 0);
    formatGroup_->addButton(oggRadio_, 1);
    formatGroup_->addButton(mp3Radio_, 2);

    // Check if MP3 is available
    if (!audio::MP3Writer::isAvailable()) {
        mp3Radio_->setEnabled(false);
        mp3Radio_->setToolTip(
            "MP3 codec not found. Copy libmp3lame.dll/dylib/so to application directory.");
    }
}

void SRenderDialog::createQualityGroup() {
    // WAV: bit depth
    wavBitDepthSpinBox_ = new QSpinBox();
    wavBitDepthSpinBox_->setMinimum(16);
    wavBitDepthSpinBox_->setMaximum(32);
    wavBitDepthSpinBox_->setSingleStep(8);
    wavBitDepthSpinBox_->setValue(16);
    wavBitDepthSpinBox_->setSuffix(" bits");

    // OGG: quality slider
    oggQualitySlider_ = new QSlider(Qt::Horizontal);
    oggQualitySlider_->setMinimum(0);
    oggQualitySlider_->setMaximum(10);
    oggQualitySlider_->setValue(6);
    oggQualitySlider_->setTickPosition(QSlider::TicksBelow);
    oggQualitySlider_->setTickInterval(1);

    // MP3: bitrate
    mp3BitrateSpinBox_ = new QSpinBox();
    mp3BitrateSpinBox_->setMinimum(128);
    mp3BitrateSpinBox_->setMaximum(320);
    mp3BitrateSpinBox_->setSingleStep(16);
    mp3BitrateSpinBox_->setValue(192);
    mp3BitrateSpinBox_->setSuffix(" kbps");
}

void SRenderDialog::createExtentGroup() {
    extentGroup_ = new QButtonGroup(this);

    entireProjectRadio_ = new QRadioButton("Entire project");
    timeSelectionRadio_ = new QRadioButton("Time selection");

    extentGroup_->addButton(entireProjectRadio_, 0);
    extentGroup_->addButton(timeSelectionRadio_, 1);
}

void SRenderDialog::createOutputGroup() {
    outputPathLineEdit_ = new QLineEdit();
    outputPathLineEdit_->setPlaceholderText("Output file path...");

    browseButton_ = new QPushButton("Browse...");
    browseButton_->setMaximumWidth(100);

    renderButton_ = new QPushButton("Render");
    cancelButton_ = new QPushButton("Cancel");
}

void SRenderDialog::onFormatChanged(int id) {
    updateQualityUI();
}

void SRenderDialog::updateQualityUI() {
    wavBitDepthSpinBox_->setVisible(wavRadio_->isChecked());
    oggQualitySlider_->setVisible(oggRadio_->isChecked());
    mp3BitrateSpinBox_->setVisible(mp3Radio_->isChecked());
}

void SRenderDialog::onBrowseClicked() {
    QString filter;
    if (wavRadio_->isChecked()) {
        filter = "WAV Files (*.wav);;All Files (*)";
    } else if (oggRadio_->isChecked()) {
        filter = "OGG Files (*.ogg);;All Files (*)";
    } else if (mp3Radio_->isChecked()) {
        filter = "MP3 Files (*.mp3);;All Files (*)";
    }

    QString fileName =
        QFileDialog::getSaveFileName(this, "Save Audio File", "", filter);
    if (!fileName.isEmpty()) {
        outputPathLineEdit_->setText(fileName);
    }
}

void SRenderDialog::onOutputPathChanged() {
    renderButton_->setEnabled(!outputPathLineEdit_->text().isEmpty());
}

bool SRenderDialog::validateInputs() {
    if (outputPathLineEdit_->text().isEmpty()) {
        QMessageBox::warning(this, "Error", "Please specify an output file.");
        return false;
    }

    if (mp3Radio_->isChecked() && !audio::MP3Writer::isAvailable()) {
        QMessageBox::warning(this, "Error",
                             "MP3 codec not available. Copy libmp3lame to app directory.");
        return false;
    }

    return true;
}

audio::RenderParams SRenderDialog::getRenderParams() const {
    audio::RenderParams params;

    // Format
    if (wavRadio_->isChecked()) {
        params.format = audio::AudioFormat::WAV;
        params.quality = wavBitDepthSpinBox_->value();
    } else if (oggRadio_->isChecked()) {
        params.format = audio::AudioFormat::OGG;
        params.quality = oggQualitySlider_->value();
    } else if (mp3Radio_->isChecked()) {
        params.format = audio::AudioFormat::MP3;
        params.quality = mp3BitrateSpinBox_->value();
    }

    // Extent
    if (timeSelectionRadio_->isChecked() && project_) {
        params.extent = audio::RenderParams::Extent::TimeSelection;
        SProject::TimeRange selection = project_->getTimeSelection();
        params.startTimeSec = selection.startSeconds;
        params.endTimeSec = selection.endSeconds;
    } else {
        params.extent = audio::RenderParams::Extent::EntireProject;
        params.startTimeSec = 0.0;
        params.endTimeSec = project_ ? project_->getDurationSeconds() : 60.0;
    }

    // Output path
    params.outputPath = outputPathLineEdit_->text().toStdString();

    return params;
}

int SRenderDialog::exec() {
    renderButton_->setEnabled(false);
    return QDialog::exec();
}
