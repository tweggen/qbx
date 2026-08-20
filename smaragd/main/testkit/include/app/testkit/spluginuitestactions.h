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
//   trackPath   = ""      index-path from the root mixer ("2", "0,1"); needed to
//                         reach a track NESTED in a folder, which `trackIndex`
//                         cannot name. Falls back to trackIndex when absent.
//   trackIndex  = "0"     legacy top-level index; still accepted
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
    QString trackPath_;          // empty = use trackIndex_
    int     trackIndex_ = 0;
    int     slotCount_  = -1;
    int     slotIndex_  = -1;
    QString contains_;
    QString absent_;
};

// assert-plugin-editor-kind — WHICH editor a double-click would open for a
// slot, without opening anything: "native" (the plugin's own GUI), "generic"
// (the slider list) or "none".
//
// This gates proposal 33 decision D3's safety property. The slider list is NOT
// reachable alongside a native GUI, so the fallback decision is the only thing
// standing between a plugin that misreports an editor and a user with no way
// to edit it at all. This verb asserts the DECISION and opens nothing;
// plugin-native-editor below is what drives a real editor, against a fixture
// that has a clap.gui and creates no window.
//   trackPath  = ""      index-path; empty = use trackIndex
//   trackIndex = "0"
//   slotIndex  = "0"
//   expect     = ""      required: "native" | "generic" | "none"
class SAssertPluginEditorKindAction : public SAction {
public:
    QString name() const override
    {
        return QStringLiteral( "assert-plugin-editor-kind" );
    }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString trackPath_;
    int     trackIndex_ = 0;
    int     slotIndex_  = 0;
    QString expect_;
};

// plugin-native-editor — open or close the PLUGIN'S OWN window (proposal 33
// M4/M6), the thing a double-click opens when a plugin has a native editor.
//
// This is the only verb that drives the app half of proposal 33 at all, and
// what makes it possible is a fixture that has a clap.gui and CREATES NO
// WINDOW (tw.test.clap.gui). The window is opened with showWindow = false, so
// nothing appears on screen — a qxa run uses the real platform plugin, not
// offscreen — while the container's native handle, the attach, the 30 Hz poll
// and every commit it makes are the production ones.
//
// A case that opens one MUST close it: an editor still alive when its plugin is
// torn down is a contract violation the backend reports as an error, and in a
// real session it is a window pointing into an unloaded DSO.
//   trackPath  = ""      index-path; empty = use trackIndex
//   trackIndex = "0"
//   slotIndex  = "0"
//   action     = "open"  "open" | "close"
//   expectOpen = "1"     what isOpenFor() must say afterwards
class SPluginNativeEditorAction : public SAction {
public:
    QString name() const override
    {
        return QStringLiteral( "plugin-native-editor" );
    }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString trackPath_;
    int     trackIndex_ = 0;
    int     slotIndex_  = 0;
    QString action_     = QStringLiteral( "open" );
    int     expectOpen_ = 1;
};

// plugin-editor-set-param — drive the parameter editor's slider the way a user
// drag does, so the resulting set-plugin-param action (and its audible effect)
// is what the render measures.
//   trackIndex = "0", slotIndex = "0", paramId = "0", value = "0"
//   expectValueText = ""   if set, the editor's value LABEL for row
//                          `expectValueRow` must equal this after the edit —
//                          proof the plugin's value-to-text reaches the screen.
//   expectValueRow  = "0"  which parameter row `expectValueText` is checked on.
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
    QString       trackPath_;          // empty = use trackIndex_
    int           trackIndex_ = 0;
    int           slotIndex_  = 0;
    std::uint32_t paramId_    = 0;
    double        value_      = 0.0;
    QString       expectValueText_;
    int           expectValueRow_ = 0;
};

// assert-instrument-slot — the MODEL side of the instrument slot (proposal 37
// P3b). Nothing here builds a widget: it asks STrack::instrumentSlot() the same
// question the engine, the head glyph and the FX strip all ask, so a case can
// pin "this track has an instrument, it is THIS one, and the processor mapped
// it THIS way" without going through a render.
//
//   trackPath  = ""      index-path from the root mixer ("2", "0,1")
//   trackIndex = "0"     legacy top-level index, used when trackPath is absent
//   present    = "1"     0 asserts there is NO instrument on the track
//   uid        = ""      the stored descriptor's uid, when given
//   format     = ""      the stored descriptor's format, when given
//   mode       = ""      the processor's channel mapping: DirectGen /
//                        MonoSpread / GenFold / WideGen (or Transparent for a
//                        layout with no defined spread)
//   state      = ""      Active / Missing / Unsupported
//   minTailFrames = "-1" if >= 0, twPlugin::tailFrames() must be at least this
//   maxTailFrames = "-1" if >= 0, it must be at most this
//   hasFeed    = "-1"    if >= 0, whether the processor holds an event source —
//                        the one thing that decides whether notes reach it
class SAssertInstrumentSlotAction : public SAction {
public:
    QString name() const override
    {
        return QStringLiteral( "assert-instrument-slot" );
    }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString trackPath_;
    int     trackIndex_ = 0;
    int     present_    = 1;
    QString uid_;
    QString format_;
    QString mode_;
    QString state_;
    long long minTailFrames_ = -1;
    long long maxTailFrames_ = -1;
    int       hasFeed_       = -1;
};

#endif  // SPLUGINUITESTACTIONS_H
