#ifndef SSETTRACKNAMEACTION_H
#define SSETTRACKNAMEACTION_H

#include "app/actions/saction.h"
#include <QList>
#include <QString>

/**
 * Action: rename a lane (the SObject SName the track head shows and the
 * project file stores).
 *
 * It exists because the head's name field was wired to NOTHING: the QLineEdit
 * carried the name at construction and no edit ever reached the model, so a
 * typed name was not saved, not undoable, and gone the moment the head was
 * rebuilt. Going through an action rather than a direct setSName() keeps it in
 * step with every other head control — mute, solo, volume and the automation
 * mode are all verbs — so one Ctrl+Z takes the name back.
 *
 * The value is ABSOLUTE: undo restores the exact previous string, because a
 * name has no meaningful increment (same rule as set-clip-name).
 *
 * Addressing follows set-track-mute: `trackPath` is an index-path from the root
 * mixer and is the only form that can name a lane NESTED in a folder track; the
 * top-level-only `trackIndex` is accepted on read for symmetry with the other
 * track verbs. `trackPath` wins when both are present.
 *
 * Deliberately NOT broadcast over the selection, unlike mute/solo/arm: those
 * apply one boolean to every selected lane, whereas one typed string on four
 * lanes would give four identically-named tracks.
 *
 * Inverse: set-track-name with the previous name.
 */
class SSetTrackNameAction : public SAction {
public:
    SSetTrackNameAction() = default;
    SSetTrackNameAction( int trackIndex, const QString &name )
        : trackIndex_( trackIndex ), name_( name ) {}
    SSetTrackNameAction( const QList<int> &trackPath, const QString &name )
        : trackPath_( trackPath ), name_( name ) {}

    QString name() const override { return QStringLiteral("set-track-name"); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    int        trackIndex_ = 0;
    QList<int> trackPath_;
    QString    name_;
};

#endif // SSETTRACKNAMEACTION_H
