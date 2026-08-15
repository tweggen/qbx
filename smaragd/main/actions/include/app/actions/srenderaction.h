#ifndef _SRENDERACTION_H
#define _SRENDERACTION_H

#include "app/actions/saction.h"
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
    double durationSec() const { return durationSec_; }

private:
    QString filename_;          // e.g., "output.wav"
    Format format_ = Format::WAV;
    int quality_ = 10;          // 0-10 for OGG/FLAC; 128-320 for MP3

    // Seconds to render, from 0. Negative (the default) = the project's
    // duration, which is what every existing case gets and is unchanged.
    //
    // Added by proposal 35 B1a for the golden corpus, and the reason is worth
    // stating: SProject::getDurationSeconds() is a hard-coded `return 60.0;`
    // with a "TODO: calculate from arrangement" beside it, so EVERY render in
    // this suite is 60 seconds long no matter what the project contains. A ~4 s
    // corpus therefore renders to 11.5 MB, 93% of it silence, and §5 asks for
    // goldens that are "a gate, not a demo". Bounding the render here keeps a
    // committed golden at 768 KB and leaves the project-duration defect exactly
    // where it was — fixing that would change the length of every render in the
    // suite, which is not this milestone's to do.
    double durationSec_ = -1.0;
};

#endif
