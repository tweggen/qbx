#ifndef _SRENDERACTION_H
#define _SRENDERACTION_H

#include <saction.h>
#include <QString>

class SRenderAction : public SAction {
public:
    enum class Format { WAV, OGG, MP3 };

    SRenderAction() = default;
    explicit SRenderAction(const QString &filename, Format format = Format::WAV,
                           int quality = 10);

    QString name() const override { return QStringLiteral("render"); }
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;

    QString filename() const { return filename_; }
    Format format() const { return format_; }
    int quality() const { return quality_; }

private:
    QString filename_;          // e.g., "output.wav"
    Format format_ = Format::WAV;
    int quality_ = 10;          // 0-10 for OGG/FLAC; 128-320 for MP3
};

#endif
