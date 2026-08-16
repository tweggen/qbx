#include "app/testkit/sassertsourcepositionaction.h"
#include "app/testkit/stestfilepath.h"
#include "app/actions/sactionregistry.h"
#include "app/shell/sapplication.h"
#include "tw/analysis/audio_analysis.h"
#include <QDomElement>
#include <QDebug>
#include <QStringList>

QStringList SAssertSourcePositionAction::knownAttributes() const
{
    return {
        QStringLiteral("filename"),
        QStringLiteral("startFrame"),
        QStringLiteral("frameCount"),
        QStringLiteral("channel"),
        QStringLiteral("expectSourceFrame"),
        QStringLiteral("tolerance"),
        QStringLiteral("expectSourceFrameAny"),
        QStringLiteral("expectSilence"),
        QStringLiteral("minConfidence"),
    };
}

SApplyResult SAssertSourcePositionAction::apply(SProject *project)
{
    if (filename_.isEmpty()) {
        qWarning() << "SAssertSourcePositionAction: no filename given";
        return {false, nullptr};
    }

    SApplication &app = SApplication::app();
    QString outputDir = app.testOutputDir();
    if (outputDir.isEmpty()) {
        qWarning() << "SAssertSourcePositionAction: no test output directory configured";
        return {false, nullptr};
    }
    const QString fullPath = resolveTestFilePath(filename_, outputDir, project);

    std::string error;
    const audio::PositionDecode d =
        audio::decodePositionAt(fullPath.toStdString(), startFrame_,
                                frameCount_, channel_, error);

    if (!error.empty()) {
        qWarning() << "SAssertSourcePositionAction: failed to decode file:"
                   << QString::fromStdString(error);
        return {false, nullptr};
    }

    // Every rejection below names the DECODED source frame. That number is the
    // entire point of the verb: "expected 40960, decoded 45056" says the audio
    // played one block late, where an RMS assertion would only have said
    // "energy out of range" — or, far worse, passed.
    const QString where = QString("%1 @ startFrame %2 (window %3)")
                              .arg(filename_).arg(startFrame_).arg(frameCount_);

    if (expectSilence_) {
        if (!d.silent) {
            qWarning() << "SAssertSourcePositionAction:" << where
                       << "expected SILENCE but decoded source frame"
                       << (qlonglong)d.sourceFrame
                       << "(block" << d.blockIndex
                       << ", confidence" << d.confidence << ")";
            return {false, nullptr};
        }
        qDebug() << "SAssertSourcePositionAction:" << where << "is silent (OK)";
        return {true, nullptr};
    }

    if (d.silent) {
        qWarning() << "SAssertSourcePositionAction:" << where
                   << "decoded SILENCE, expected position-coded audio"
                   << (expectSourceFrame_ >= 0
                           ? QString("(expected source frame %1)").arg(expectSourceFrame_)
                           : QString());
        return {false, nullptr};
    }

    if (d.confidence < minConfidence_) {
        // Almost always a window straddling a block boundary: the decoder saw
        // two positions at once and is refusing to pick one. Say so, because
        // the fix is usually to align the window, not to loosen the bound.
        qWarning() << "SAssertSourcePositionAction:" << where
                   << "AMBIGUOUS: confidence" << d.confidence
                   << "below minConfidence" << minConfidence_
                   << "- best guess was source frame" << (qlonglong)d.sourceFrame
                   << "(a window straddling a" << (qlonglong)4096
                   << "-frame block boundary decodes two positions at once)";
        return {false, nullptr};
    }

    if (!expectSourceFrameAny_.isEmpty()) {
        const QStringList parts = expectSourceFrameAny_.split(',', Qt::SkipEmptyParts);
        for (const QString &p : parts) {
            bool ok = false;
            const qlonglong want = p.trimmed().toLongLong(&ok);
            if (ok && qAbs((qlonglong)d.sourceFrame - want) <= (qlonglong)tolerance_) {
                qDebug() << "SAssertSourcePositionAction:" << where
                         << "decoded source frame" << (qlonglong)d.sourceFrame
                         << "matches accepted position" << want
                         << "(confidence" << d.confidence << ")";
                return {true, nullptr};
            }
        }
        qWarning() << "SAssertSourcePositionAction:" << where
                   << "decoded source frame" << (qlonglong)d.sourceFrame
                   << "(block" << d.blockIndex << ") is not any of the accepted"
                   << "positions [" << expectSourceFrameAny_ << "] +/-"
                   << (qlonglong)tolerance_
                   << "- confidence" << d.confidence;
        return {false, nullptr};
    }

    if (expectSourceFrame_ >= 0) {
        const qlonglong delta = (qlonglong)d.sourceFrame - (qlonglong)expectSourceFrame_;
        if (qAbs(delta) > (qlonglong)tolerance_) {
            qWarning() << "SAssertSourcePositionAction:" << where
                       << "decoded source frame" << (qlonglong)d.sourceFrame
                       << "(block" << d.blockIndex << ") but expected"
                       << (qlonglong)expectSourceFrame_ << "+/-"
                       << (qlonglong)tolerance_ << "- off by" << delta
                       << "frames; confidence" << d.confidence;
            return {false, nullptr};
        }
        qDebug() << "SAssertSourcePositionAction:" << where
                 << "decoded source frame" << (qlonglong)d.sourceFrame
                 << "(expected" << (qlonglong)expectSourceFrame_
                 << ", confidence" << d.confidence << ") OK";
        return {true, nullptr};
    }

    // Neither expectation given: the decode itself (audible, unambiguous) is
    // all that was asked for.
    qDebug() << "SAssertSourcePositionAction:" << where
             << "decoded source frame" << (qlonglong)d.sourceFrame
             << "(confidence" << d.confidence << ") - no expectation given";
    return {true, nullptr};
}

void SAssertSourcePositionAction::writeXml(QDomElement &elem) const
{
    elem.setAttribute("filename", filename_);
    elem.setAttribute("startFrame", QString::number(startFrame_));
    elem.setAttribute("frameCount", QString::number(frameCount_));
    elem.setAttribute("tolerance", QString::number(tolerance_));
    elem.setAttribute("minConfidence", QString::number(minConfidence_, 'f', 3));
    if (channel_ != -1) {
        elem.setAttribute("channel", QString::number(channel_));
    }
    if (expectSourceFrame_ >= 0) {
        elem.setAttribute("expectSourceFrame", QString::number(expectSourceFrame_));
    }
    if (!expectSourceFrameAny_.isEmpty()) {
        elem.setAttribute("expectSourceFrameAny", expectSourceFrameAny_);
    }
    if (expectSilence_) {
        elem.setAttribute("expectSilence", "true");
    }
}

bool SAssertSourcePositionAction::readXml(const QDomElement &elem, int /*version*/)
{
    // A missing filename is reported by apply(), not here: readXml failing
    // makes the action undeserializable, which breaks the round-trip audit.
    filename_ = elem.attribute("filename", "");

    bool ok1, ok2, ok3, ok4, ok5, ok6;
    startFrame_        = elem.attribute("startFrame", "0").toLongLong(&ok1);
    frameCount_        = elem.attribute("frameCount", "4096").toLongLong(&ok2);
    channel_           = elem.attribute("channel", "-1").toInt(&ok3);
    expectSourceFrame_ = elem.attribute("expectSourceFrame", "-1").toLongLong(&ok4);
    tolerance_         = elem.attribute("tolerance", "4096").toLongLong(&ok5);
    minConfidence_     = elem.attribute("minConfidence", "3.0").toDouble(&ok6);
    expectSourceFrameAny_ = elem.attribute("expectSourceFrameAny", "");
    expectSilence_     = elem.attribute("expectSilence", "false") == "true";

    if (!ok1 || !ok2 || !ok3 || !ok4 || !ok5 || !ok6) {
        qWarning() << "SAssertSourcePositionAction::readXml: invalid numeric attributes";
        return false;
    }
    return true;
}

static const bool s_reg_assert_source_position = (
    SActionRegistry::instance().registerType(
        QStringLiteral("assert-source-position"),
        []{ return new SAssertSourcePositionAction; }
    ), true
);
