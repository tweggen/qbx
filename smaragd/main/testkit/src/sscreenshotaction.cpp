#include "app/testkit/sscreenshotaction.h"
#include "app/shell/sapplication.h"
#include "app/actions/sactionregistry.h"
#include <QDomElement>
#include <QScreen>
#include <QApplication>
#include <QPixmap>
#include <QDebug>

SScreenshotAction::SScreenshotAction(const QString &filename, Resolution resolution,
                                       int customWidth, int customHeight)
    : filename_(filename), resolution_(resolution), customWidth_(customWidth), customHeight_(customHeight)
{
}

SApplyResult SScreenshotAction::apply(SProject *project)
{
    // Validate preconditions
    if (filename_.isEmpty()) {
        return {false, nullptr};
    }

    // Sanitize filename: reject paths with / or .. to prevent directory traversal
    if (filename_.contains("/") || filename_.contains("\\") || filename_.contains("..")) {
        qWarning() << "SScreenshotAction: filename contains path separators:" << filename_;
        return {false, nullptr};
    }

    SApplication &app = SApplication::app();
    QString outputDir = app.testOutputDir();
    if (outputDir.isEmpty()) {
        qWarning() << "SScreenshotAction: no test output directory configured";
        return {false, nullptr};
    }

    // Ensure output directory exists
    if (!app.ensureOutputDirExists()) {
        qWarning() << "SScreenshotAction: failed to create output directory:" << outputDir;
        return {false, nullptr};
    }

    // Grab the primary screen's window
    QScreen *screen = QApplication::primaryScreen();
    if (!screen) {
        qWarning() << "SScreenshotAction: no primary screen available";
        return {false, nullptr};
    }

    QPixmap pixmap = screen->grabWindow(0);  // 0 = root window
    if (pixmap.isNull()) {
        qWarning() << "SScreenshotAction: failed to grab window";
        return {false, nullptr};
    }

    // Determine target resolution
    QSize targetSize = pixmap.size();
    switch (resolution_) {
        case Resolution::Full:
            // Use pixmap size as-is
            break;
        case Resolution::Half:
            // 50% width/height
            targetSize = pixmap.size() / 2;
            break;
        case Resolution::Custom:
            // Use custom width/height
            if (customWidth_ > 0 && customHeight_ > 0) {
                targetSize = QSize(customWidth_, customHeight_);
            }
            break;
    }

    // Scale if necessary
    if (targetSize != pixmap.size()) {
        pixmap = pixmap.scaledToWidth(targetSize.width(), Qt::SmoothTransformation);
    }

    // Construct full output path
    QString fullPath = outputDir + "/" + filename_;

    // Save as PNG
    if (!pixmap.save(fullPath, "PNG")) {
        qWarning() << "SScreenshotAction: failed to save screenshot to" << fullPath;
        return {false, nullptr};
    }

    qDebug() << "SScreenshotAction: saved screenshot to" << fullPath;
    return {true, nullptr};  // Screenshots are not undoable
}

void SScreenshotAction::writeXml(QDomElement &elem) const
{
    elem.setAttribute("filename", filename_);

    QString resolutionStr;
    switch (resolution_) {
        case Resolution::Full:
            resolutionStr = "100%";
            break;
        case Resolution::Half:
            resolutionStr = "50%";
            break;
        case Resolution::Custom:
            resolutionStr = QString::number(customWidth_) + "x" + QString::number(customHeight_);
            break;
    }
    elem.setAttribute("resolution", resolutionStr);
}

bool SScreenshotAction::readXml(const QDomElement &elem, int /*version*/)
{
    filename_ = elem.attribute("filename", "");
    // Note: empty filename is allowed for roundtrip testing; apply() will reject it

    QString resolutionStr = elem.attribute("resolution", "100%");
    if (resolutionStr == "100%") {
        resolution_ = Resolution::Full;
    } else if (resolutionStr == "50%") {
        resolution_ = Resolution::Half;
    } else if (resolutionStr.contains("x")) {
        // Parse custom resolution like "800x600"
        QStringList parts = resolutionStr.split("x");
        if (parts.length() == 2) {
            bool ok1, ok2;
            int w = parts[0].toInt(&ok1);
            int h = parts[1].toInt(&ok2);
            if (ok1 && ok2 && w > 0 && h > 0) {
                resolution_ = Resolution::Custom;
                customWidth_ = w;
                customHeight_ = h;
            } else {
                qWarning() << "SScreenshotAction::readXml: invalid custom resolution:" << resolutionStr;
                return false;
            }
        } else {
            qWarning() << "SScreenshotAction::readXml: invalid resolution format:" << resolutionStr;
            return false;
        }
    } else {
        qWarning() << "SScreenshotAction::readXml: unknown resolution:" << resolutionStr;
        return false;
    }

    return true;
}

static const bool s_reg_screenshot = (
    SActionRegistry::instance().registerType(
        QStringLiteral("screenshot"),
        []{ return new SScreenshotAction; }
    ), true
);
