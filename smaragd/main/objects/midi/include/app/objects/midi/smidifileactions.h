#ifndef _SMIDIFILEACTIONS_H_
#define _SMIDIFILEACTIONS_H_

#include <QList>
#include <QString>

#include "app/actions/saction.h"
#include "tw/core/twtypes.h"

/**
 * `import-midi-file` - one `SMidiSequence` per SMF track, materialised INLINE.
 *
 * The file's PPQ is rescaled to the house 960 (D2). A `.mid` is a source, not
 * a live reference: once imported, the notes live in the project file, so they
 * can never go missing the way a sample can. The path is stored through
 * `SFilePathRef` only for the record of where it came from.
 *
 * TEMPO. The first tempo meta becomes a `set-tempo` when the project is still
 * empty, and otherwise only warns - importing a file must never silently
 * re-time everything the user already has. Every tempo event, the first one
 * included, ALSO stays in the sequence as a `Tempo` metadata event: dropping
 * it would make `export-midi-file` write a different file than it read, and a
 * conductor track is data the file carried. Timing itself stays constant until
 * proposal 37 (tempo segments), which is warned about once.
 *
 * `mode`:   tracks   - one clip per SMF track (the default)
 *           merged   - every track folded into one clip
 * `newTracks="1"` creates project tracks as needed, starting at `trackPath`.
 */
class SImportMidiFileAction : public SAction
{
public:
    SImportMidiFileAction() = default;
    SImportMidiFileAction( const QList<int> &trackPath, const QString &filePath,
                           offset_t timePos, const QString &mode,
                           bool newTracks );

    QString name() const override { return QStringLiteral( "import-midi-file" ); }
    QStringList knownAttributes() const override
    { return { "trackPath", "filePath", "timePos", "mode", "newTracks" }; }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QList<int> trackPath_;
    QString    filePath_;
    offset_t   timePos_ = 0;
    QString    mode_ = QStringLiteral( "tracks" );
    bool       newTracks_ = true;
};

/**
 * `export-midi-file` - write a clip, a track, or the whole project as an SMF.
 *
 * Not undoable (it writes a file, it does not change the project). Track order
 * is the arrangement's own order, one SMF track per event clip, so a file
 * imported and exported straight back is byte-identical - which is the gate
 * (`assert-file-identical`), because twSmf has exactly one canonical spelling
 * (events/CONTRACT inv. 15).
 */
class SExportMidiFileAction : public SAction
{
public:
    SExportMidiFileAction() = default;
    SExportMidiFileAction( const QString &filePath, const QList<int> &clipPath,
                           const QList<int> &trackPath, int type );

    QString name() const override { return QStringLiteral( "export-midi-file" ); }
    QStringList knownAttributes() const override
    { return { "filePath", "clip", "trackPath", "type" }; }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString    filePath_;
    QList<int> clipPath_;    // empty = not addressed
    QList<int> trackPath_;   // empty = not addressed
    bool       hasClip_ = false;
    bool       hasTrack_ = false;
    int        type_ = 1;
};

#endif // _SMIDIFILEACTIONS_H_
