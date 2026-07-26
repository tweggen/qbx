#ifndef SPLUGINUITESTACTIONS_H
#define SPLUGINUITESTACTIONS_H

#include "app/actions/saction.h"

#include <QString>
#include <cstdint>

// Headless coverage for the proposal 08 M5 plugin UI.
//
// M5 is almost entirely widget work, and this repo has no way to look at a
// window. These two verbs build the REAL widgets (SPluginEffectStrip and the
// SPluginParamEditor it opens) over a real track, off screen and unshown, and
// assert on what they rendered / what they submitted. Without them the greying
// of a Missing slot and the editor -> action wiring would have no automated
// coverage at all.
//
// They never synthesize OS input and never show a window: on Windows a qxa run
// uses the real platform plugin, so a shown dialog would appear on the user's
// screen and could steal focus.

// assert-plugin-strip — build the FX strip for a track and assert on a row.
//   trackIndex  = "0"     which track's strip to build
//   slotCount   = "-1"    if >= 0, the number of rows the strip rendered
//   slotIndex   = "-1"    which row `contains` is matched against
//   contains    = ""      substring that must appear in
//                         SPluginEffectStrip::describeSlot(slotIndex)
//   absent      = ""      substring that must NOT appear there
class SAssertPluginStripAction : public SAction {
public:
    QString name() const override { return QStringLiteral( "assert-plugin-strip" ); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    int     trackIndex_ = 0;
    int     slotCount_  = -1;
    int     slotIndex_  = -1;
    QString contains_;
    QString absent_;
};

// plugin-editor-set-param — drive the parameter editor's slider the way a user
// drag does, so the resulting set-plugin-param action (and its audible effect)
// is what the render measures.
//   trackIndex = "0", slotIndex = "0", paramId = "0", value = "0"
class SPluginEditorSetParamAction : public SAction {
public:
    QString name() const override
    {
        return QStringLiteral( "plugin-editor-set-param" );
    }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    int           trackIndex_ = 0;
    int           slotIndex_  = 0;
    std::uint32_t paramId_    = 0;
    double        value_      = 0.0;
};

#endif  // SPLUGINUITESTACTIONS_H
