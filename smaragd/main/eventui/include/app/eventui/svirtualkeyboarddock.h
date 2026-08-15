#ifndef _SVIRTUALKEYBOARDDOCK_H_
#define _SVIRTUALKEYBOARDDOCK_H_

#include <QList>
#include <QString>
#include <QWidget>

class QLabel;
class QSpinBox;
class QToolButton;

/**
 * SVirtualKeyboardDock - two painted octaves driven by the computer keyboard
 * (proposal 36 6.3).
 *
 * The key map is REAPER's, because it is the one a user coming from any other
 * DAW already has in their fingers:
 *
 *     lower octave   Z S X D C V G B H N J M   (C .. B)
 *     upper octave   Q 2 W 3 E R 5 T 6 Y 7 U   (C .. B, one octave up)
 *
 * SPACE IS NOT OURS. It is the transport, everywhere, always - so an unmapped
 * key (Space included) is ignore()d and propagates to the shell's shortcut.
 * A widget that swallowed it would silently break play/stop whenever the
 * keyboard happened to have focus, which is the single most annoying bug a
 * virtual keyboard can have.
 *
 * A press INSERTS A NOTE at the locator through `add-note` (proposal 36 3.4) -
 * it does not sound anything yet; live preview through an instrument is P3b/P8
 * work. That is also what makes it the headless note source behind the
 * `virtual-key` verb: one entry point, one action, one undo step.
 */
class SVirtualKeyboardDock : public QWidget
{
    Q_OBJECT
public:
    explicit SVirtualKeyboardDock( QWidget *parent = nullptr );
    ~SVirtualKeyboardDock() override;

    /** MIDI note for a computer key, or -1 when the key is not on the map. */
    int noteForKey( int qtKey ) const;

    /**
     * Insert one note at the locator, into the selected event clip (or the
     * first one on the selected track). Returns false - and touches nothing -
     * when there is no event clip to write into, which is what makes
     * `virtual-key` REJECT rather than silently do nothing.
     */
    bool pressNote( int midiKey, double velocity, qint64 durationTicks );

    int  octave() const { return octave_; }
    void setOctave( int octave );
    double velocity() const;

    /** Test face: `octave=4|velocity=100|keys=24|last=60`. */
    QString describe() const;

protected:
    void paintEvent( QPaintEvent * ) override;
    void mousePressEvent( QMouseEvent * ) override;
    void keyPressEvent( QKeyEvent * ) override;

private:
    void buildUi();
    /** The MIDI note under a point on the painted keyboard, or -1. */
    int  noteAtPoint( const QPoint &p ) const;
    int  baseKey() const { return 12 * ( octave_ + 1 ); }

    QSpinBox    *velocitySpin_ = nullptr;
    QToolButton *octaveDown_   = nullptr;
    QToolButton *octaveUp_     = nullptr;
    QLabel      *octaveLabel_  = nullptr;

    int octave_  = 4;      // octave 4 => C4 = MIDI 60
    int lastKey_ = -1;     // for describe() and for the pressed-key highlight
    /** Where the painted keyboard starts, under the little control row. */
    static constexpr int CONTROLS_H = 22;
    static constexpr int OCTAVES    = 2;
};

#endif // _SVIRTUALKEYBOARDDOCK_H_
