#ifndef SSAMPLERVIEWS_H
#define SSAMPLERVIEWS_H

// ssamplerviews.h — the editor's four views (proposal 56 §4.11; nassau-digital
// DESIGN §7; QBX-279). Each is a FIXED-SIZE widget (setFixedSize in its
// constructor, the size SSamplerEditor::*ViewSize() names) painting from
// ssamplerzones over the instance's program, in the application palette (the
// app theme sets it). Key, Pad and Map display and select only; the Zone view's
// knobs are the editing surface (the author, 2026-11-03).
//
// Every view takes a drop of `application/x-smaragd-resource` and resolves the
// position to a key, a pad's key or a zone with ITS OWN hit test, then hands it
// to SSamplerEditor::dropOn().

#include <QPoint>
#include <QString>
#include <QVector>
#include <QWidget>

#include <cstdint>

class QButtonGroup;
class QDial;
class QLabel;
class QPushButton;
class SSamplerEditor;

// What every view shares: the editor, the drop handling, the last drop's
// outcome (for the seam).
class SSamplerViewBase : public QWidget {
    Q_OBJECT
public:
    explicit SSamplerViewBase( SSamplerEditor *editor, QWidget *parent = nullptr );
    virtual void refreshFromModel() { update(); }

    bool    lastDropSubmitted() const { return lastDropOk_; }
    QString lastDropWhy() const { return lastDropWhy_; }

protected:
    void dragEnterEvent( QDragEnterEvent *e ) override;
    void dragMoveEvent( QDragMoveEvent *e ) override;
    void dropEvent( QDropEvent *e ) override;
    // Resolve a drop position: a key (>= 0) or a zone id; false = nothing there.
    virtual bool dropTargetAt( const QPoint &pos, int *key, std::uint32_t *zone ) const = 0;

    SSamplerEditor *editor_;
    bool            lastDropOk_ = false;
    QString         lastDropWhy_;
};

// KEY: the keyboard, 128 keys, the zones drawn as bars above it (selected
// ones in the highlight colour), the held keys marked. A press selects the
// zones covering the key (select-zones).
class SSamplerKeyView : public SSamplerViewBase {
    Q_OBJECT
public:
    explicit SSamplerKeyView( SSamplerEditor *editor, QWidget *parent = nullptr );
    static constexpr int kKeyWidth   = 7;
    static constexpr int kZoneTop    = 18;   // under the octave labels
    static constexpr int kZoneRow    = 9;
    static constexpr int kZoneRows   = 6;
    static constexpr int kKeysTop    = 76;
    // The centre of `key` on the keyboard, in this widget's coordinates.
    static QPoint keyCentre( int key );
    static int    keyAt( const QPoint &pos );

protected:
    void paintEvent( QPaintEvent *e ) override;
    void mousePressEvent( QMouseEvent *e ) override;
    bool dropTargetAt( const QPoint &pos, int *key, std::uint32_t *zone ) const override;
};

// PAD: a 4×4 grid (pad 1 bottom left, as on an MPC) and the bank switcher
// A..F. A pad cell shows its name, its key and how many zones cover the key;
// a pad with no key is drawn empty and takes no click. A click selects the pad
// (set-sampler-view padBank pad); a bank button switches bank.
class SSamplerPadView : public SSamplerViewBase {
    Q_OBJECT
public:
    explicit SSamplerPadView( SSamplerEditor *editor, QWidget *parent = nullptr );
    static constexpr int kCell   = 88;
    static constexpr int kGap    = 8;
    static constexpr int kGridTop = 48;
    static QRect cellRect( int pad );
    static int   padAt( const QPoint &pos );
    bool clickBank( int bank );
    void refreshFromModel() override;

protected:
    void paintEvent( QPaintEvent *e ) override;
    void mousePressEvent( QMouseEvent *e ) override;
    bool dropTargetAt( const QPoint &pos, int *key, std::uint32_t *zone ) const override;

private:
    QButtonGroup *banks_ = nullptr;
    QPushButton  *bankButton_[6] = {};
};

// MAP: key (x) × velocity (y, 127 at the top), every zone a rectangle, the
// selected ones in the highlight colour, each VELOCITY GAP (ssamplerzones)
// hatched across its zone's keys. A press selects the zones whose rectangle
// holds the point (none: an empty selection).
class SSamplerMapView : public SSamplerViewBase {
    Q_OBJECT
public:
    explicit SSamplerMapView( SSamplerEditor *editor, QWidget *parent = nullptr );
    static constexpr int kLeft   = 32;
    static constexpr int kTop    = 8;
    static constexpr int kKeyW   = 6;
    static constexpr int kVelH   = 3;
    static QPoint pointFor( int key, int vel );
    static bool   cellAt( const QPoint &pos, int *key, int *vel );

protected:
    void paintEvent( QPaintEvent *e ) override;
    void mousePressEvent( QMouseEvent *e ) override;
    bool dropTargetAt( const QPoint &pos, int *key, std::uint32_t *zone ) const override;
};

// ZONE: the knob rows of the zone the selection names first — Amp, Drive,
// Filter, Envelope, and Layers (level, tune, pan of layers 1..4). A knob spans
// the parameter's range in the core's table; turning it submits set-zone for
// EVERY selected zone (DESIGN §2.1's "the selected zones" scope) or set-layer
// for that layer, absolute. One drag is one undo step: the knob mints a
// gesture token on press and stamps every action until release (QBX-206), and
// the set verbs merge on it.
class SSamplerZoneView : public SSamplerViewBase {
    Q_OBJECT
public:
    explicit SSamplerZoneView( SSamplerEditor *editor, QWidget *parent = nullptr );
    void refreshFromModel() override;

    struct Knob {
        QString       id;        // "zone.cutoff", "layer2.level"
        bool          layer = false;
        int           layerIndex = 0;   // 0..3 for a layer knob
        QString       attr;
        double        lo = 0.0, hi = 1.0;
        bool          integer = false;
        bool          logScale = false;
        QDial        *dial  = nullptr;
        QLabel       *value = nullptr;
        quint64       gesture = 0;
    };
    const QVector<Knob> &knobs() const { return knobs_; }
    QDial *dialFor( const QString &id ) const;
    // Set `id` to `value` (parameter units) through the dial.
    bool setKnob( const QString &id, double value );
    std::uint32_t shownZone() const { return shownZone_; }
    QString describeKnobs() const;

protected:
    bool dropTargetAt( const QPoint &pos, int *key, std::uint32_t *zone ) const override;

private:
    void onDial_( int index );
    int    ticksFor_( const Knob &k, double v ) const;
    double valueFor_( const Knob &k, int ticks ) const;

    QVector<Knob>  knobs_;
    QLabel        *header_ = nullptr;
    std::uint32_t  shownZone_ = 0;
    bool           applying_ = false;
};

#endif
