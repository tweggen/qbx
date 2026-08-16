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
    // An EXPLICIT bound on how much audio a case renders, independent of what
    // the project's own duration says. A case that only asserts the first two
    // seconds of an arrangement should not have to write (and byte-compare, and
    // wait for) the rest of it.
    //
    // Deliberately not a fix for SProject::getDurationSeconds(), which is its
    // own question: this narrows a render, it never widens one. Setting it
    // longer than the arrangement gets silence, exactly as rendering the whole
    // project past its last clip does.
    double durationSec_ = -1.0;
};

#endif
