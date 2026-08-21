#ifndef STHEMECOLORS_H
#define STHEMECOLORS_H

#include <QColor>
#include <QPalette>

// The theme's COLOUR TOKENS, and the one place they are named.
//
// Every colour the style paints comes from here, by ROLE rather than by hex.
// That is what makes a second theme (a light one, a user-chosen accent) a new
// factory function rather than a second style class -- SBrownProStyle holds an
// SThemeColors by value and never mentions a literal colour.
//
// It is deliberately NOT a QPalette. A QPalette has 20 roles chosen for the
// widgets Qt shipped in 1998; it has no way to say "the fill a control takes
// when the pointer is over it" or "the line between a toolbar and the canvas",
// which are exactly the distinctions a modern look is made of. palette() below
// projects these tokens DOWN onto a QPalette, because Qt needs one and because
// every widget that draws itself (and every stylesheet still in the tree) reads
// that projection rather than this struct.
struct SThemeColors
{
    // --- surfaces, deepest to lightest -----------------------------------
    QColor windowBg;        // the ground: window, dock and canvas backdrop
    QColor surface;         // a control at rest: button, combo, tab
    QColor surfaceRaised;   // chrome that sits ON the ground: toolbar, header
    QColor surfaceSunken;   // things you type or scroll INTO: fields, grooves
    QColor hover;           // pointer over a control
    QColor pressed;         // control held down, or checked-but-not-accented
    QColor surfaceDisabled; // a control that is present but unavailable

    // surfaceDisabled is NOT windowBg, and that is the whole reason it exists.
    // Filling a disabled control with the ground colour makes it DISAPPEAR
    // rather than read as unavailable -- measured on the widget gallery: a
    // disabled line edit and a disabled slider handle were invisible, so the
    // slider looked like a broken track with a gap in it. Unavailable still has
    // a body; only its ink and its outline recede.

    // --- lines -------------------------------------------------------------
    QColor border;          // a control's own outline
    QColor borderStrong;    // the outline of something focused or raised
    QColor divider;         // separators, splitters, grid lines

    // --- ink ----------------------------------------------------------------
    QColor text;
    QColor textDim;         // secondary labels, placeholder, shortcut column
    QColor textDisabled;
    QColor textOnAccent;    // ink laid ON accent/accentAlt

    // --- accents -------------------------------------------------------------
    QColor accent;          // orange: checked, focus, selection, active tab
    QColor accentHover;
    QColor accentMuted;     // accent at rest behind text (selection in a list)
    QColor accentAlt;       // blue: progress and other informational fills
    QColor warning;
    QColor error;

    // The shipped dark theme. Named for the warm brown-grey ground that makes
    // it recognisable at a glance next to a stock Fusion window -- which is the
    // whole point of having a style at all.
    static SThemeColors brownProDark();

    // Project these tokens onto Qt's own 20 roles, including the Disabled and
    // Inactive groups. Without the Disabled group a dark palette leaves greyed
    // text at the platform default, which on a #2E2A28 ground is unreadable.
    QPalette palette() const;
};

#endif // STHEMECOLORS_H
