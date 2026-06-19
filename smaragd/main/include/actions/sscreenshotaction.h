#ifndef _SSCREENSHOTACTION_H
#define _SSCREENSHOTACTION_H

#include <saction.h>
#include <QString>

class SScreenshotAction : public SAction {
public:
    enum class Resolution { Full, Half, Custom };

    SScreenshotAction() = default;
    explicit SScreenshotAction(const QString &filename, Resolution resolution = Resolution::Full,
                                int customWidth = 0, int customHeight = 0);

    QString name() const override { return QStringLiteral("screenshot"); }
    SApplyResult apply(SProject *project) override;
    void writeXml(QDomElement &elem) const override;
    bool readXml(const QDomElement &elem, int version) override;

    QString filename() const { return filename_; }
    Resolution resolution() const { return resolution_; }
    int customWidth() const { return customWidth_; }
    int customHeight() const { return customHeight_; }

private:
    QString filename_;
    Resolution resolution_ = Resolution::Full;
    int customWidth_ = 0;
    int customHeight_ = 0;
};

#endif
