#include "app/theme/sthemecolors.h"

SThemeColors SThemeColors::brownProDark()
{
    SThemeColors c;

    // The six surface tokens are one ramp, deliberately close together: the
    // separation that makes a control readable comes from its BORDER, not from
    // a big fill contrast, which is what keeps a dense arranger from looking
    // like a chessboard.
    c.windowBg      = QColor( "#2E2A28" );
    c.surface       = QColor( "#3C3634" );   // the sketch's base
    c.surfaceRaised = QColor( "#453F3D" );
    c.surfaceSunken = QColor( "#2A2624" );
    c.hover         = QColor( "#524C4A" );   // the sketch's hover
    c.pressed       = QColor( "#5C5654" );   // the sketch's active
    c.surfaceDisabled = QColor( "#37322F" );  // between windowBg and surface

    c.border        = QColor( "#4A4442" );   // the sketch's frame
    c.borderStrong  = QColor( "#6B6360" );
    c.divider       = QColor( "#3A3432" );

    c.text          = QColor( "#E6E0DD" );   // the sketch's text
    c.textDim       = QColor( "#B0A8A4" );
    c.textDisabled  = QColor( "#8C8683" );   // the sketch's disabled
    c.textOnAccent  = QColor( "#211D1B" );

    c.accent        = QColor( "#D97A2F" );   // the sketch's accentOrange
    c.accentHover   = QColor( "#E88C3F" );
    c.accentMuted   = QColor( "#8A4C1D" );
    c.accentAlt     = QColor( "#7FAACC" );   // the sketch's accentBlue
    c.warning       = QColor( "#D9A62F" );
    c.error         = QColor( "#D95A4A" );

    return c;
}

QPalette SThemeColors::palette() const
{
    QPalette p;

    // --- Active (the focused window) ------------------------------------
    p.setColor( QPalette::Window,          windowBg );
    p.setColor( QPalette::WindowText,      text );
    p.setColor( QPalette::Base,            surfaceSunken );
    p.setColor( QPalette::AlternateBase,   surface );
    p.setColor( QPalette::ToolTipBase,     surfaceRaised );
    p.setColor( QPalette::ToolTipText,     text );
    p.setColor( QPalette::PlaceholderText, textDim );
    p.setColor( QPalette::Text,            text );
    p.setColor( QPalette::Button,          surface );
    p.setColor( QPalette::ButtonText,      text );
    p.setColor( QPalette::BrightText,      error );
    p.setColor( QPalette::Link,            accentAlt );
    p.setColor( QPalette::LinkVisited,     accentMuted );
    p.setColor( QPalette::Highlight,       accent );
    p.setColor( QPalette::HighlightedText, textOnAccent );

    // Qt's 3D roles still drive QFrame's Sunken/Raised shadows and a handful of
    // widgets that draw their own bevels, so they are set rather than left at
    // the default grey ramp -- a stray light-grey hairline on a dark ground is
    // the commonest tell of an incompletely themed app.
    p.setColor( QPalette::Light,    surfaceRaised );
    p.setColor( QPalette::Midlight, border );
    p.setColor( QPalette::Dark,     surfaceSunken );
    p.setColor( QPalette::Mid,      divider );
    p.setColor( QPalette::Shadow,   QColor( "#1B1817" ) );

    // --- Disabled -----------------------------------------------------------
    p.setColor( QPalette::Disabled, QPalette::WindowText,      textDisabled );
    p.setColor( QPalette::Disabled, QPalette::Text,            textDisabled );
    p.setColor( QPalette::Disabled, QPalette::ButtonText,      textDisabled );
    p.setColor( QPalette::Disabled, QPalette::Base,            surfaceDisabled );
    p.setColor( QPalette::Disabled, QPalette::Button,          surfaceDisabled );
    p.setColor( QPalette::Disabled, QPalette::Highlight,       divider );
    p.setColor( QPalette::Disabled, QPalette::HighlightedText, textDisabled );
    p.setColor( QPalette::Disabled, QPalette::Light,           divider );

    // --- Inactive (an unfocused window) -------------------------------------
    // Only the selection dims; everything else stays put, so clicking away from
    // the arranger does not repaint the whole app a shade darker.
    p.setColor( QPalette::Inactive, QPalette::Highlight,       accentMuted );
    p.setColor( QPalette::Inactive, QPalette::HighlightedText, text );

    return p;
}
