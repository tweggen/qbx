#ifndef SPLACECLIPACTION_H
#define SPLACECLIPACTION_H

#include "app/actions/saction.h"
#include "tw/graph/tw303aenv.h"
#include <QList>
#include <QString>

// Action: place a file as a WINDOWED plain cut on a lane addressed by path
// (proposal 17 phase 2). The path-addressed, windowed sibling of add-sample
// (which is root-track-index-only and always places the full wave):
// `startOffset` slips into the source, `duration` 0 = the full wave.
// The recording planner (place-recording) uses this for the gap segments.
//
// Inverse: SUnplaceClipAction (below), whose own inverse re-places.
class SPlaceClipAction : public SAction {
public:
    SPlaceClipAction() = default;
    SPlaceClipAction( const QList<int> &trackPath, const QString &filePath,
                      offset_t timePos, offset_t startOffset = 0,
                      length_t duration = 0 );

    QString name() const override { return QStringLiteral("place-clip"); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QList<int> trackPath_;
    QString    filePath_;
    offset_t   timePos_ = 0;
    offset_t   startOffset_ = 0;
    length_t   duration_ = 0;    // 0 = full wave
};

// Live-only inverse of SPlaceClipAction: delete the placed clip.
class SUnplaceClipAction : public SAction {
public:
    SUnplaceClipAction( const QList<int> &clipPath,
                        const QList<int> &trackPath, const QString &filePath,
                        offset_t timePos, offset_t startOffset,
                        length_t duration );

    QString name() const override { return QStringLiteral("unplace-clip"); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QList<int> clipPath_;
    QList<int> trackPath_;
    QString    filePath_;
    offset_t   timePos_ = 0;
    offset_t   startOffset_ = 0;
    length_t   duration_ = 0;
};

#endif // SPLACECLIPACTION_H
