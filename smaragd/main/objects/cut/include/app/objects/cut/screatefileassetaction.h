#ifndef SCREATEFILEASSETACTION_H
#define SCREATEFILEASSETACTION_H

#include "app/actions/saction.h"
#include "tw/graph/tw303aenv.h"
#include <QString>

// create-file-asset — register an AUDIO FILE as a qbx asset (proposal 56 Q6,
// the author's Q-4 answer of 2026-11-01: a recording made in the sampler is a
// WAV under the recordings folder AND a registered asset, so it can also be
// placed on a lane).
//
// The asset body is an SCut over the file's SExternFile (the object
// `place-recording` cuts from), windowing [srcOffset, srcOffset + length)
// (length 0 = to the file's end). `assetName` empty = the first unused
// "Recording N"; the FIRST apply remembers the name it took, so a redo
// registers the same name again. Refused, changing nothing: no file, a file
// that does not open as audio, an offset at or past its end, a name in use.
//
// The inverse is `remove-asset`, built through the REGISTRY (objects/mixer
// owns it and this module may not name it); `remove-asset` of a file-backed
// asset in turn inverts to this verb.
class SCreateFileAssetAction : public SAction {
public:
    SCreateFileAssetAction() = default;
    SCreateFileAssetAction( const QString &filePath, offset_t srcOffset, length_t length,
                            const QString &assetName );

    QString name() const override { return QStringLiteral( "create-file-asset" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    QStringList knownAttributes() const override;

private:
    QString  filePath_;
    offset_t srcOffset_ = 0;
    length_t length_    = 0;
    QString  assetName_;
};

#endif
