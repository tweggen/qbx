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
//   strayWindows = "-1"   if >= 0, how many widgets were SHOWN while still
//                         top-level while the strip was being built. The
//                         answer must be 0 and the default is "do not check"
//                         (QBX-117): a widget made visible before its layout
//                         has it is a TOP-LEVEL WINDOW for that instant, Qt
//                         gives it a real platform window, and on macOS AppKit
//                         can move the main window's full-screen Space onto it
//                         — which took a full-screened Smaragd out of full
//                         screen every time a track with a plugin was selected.
//                         Nothing else in the repo can see that: the debris is
//                         invisible on Windows and X11, and it is gone again by
//                         the time apply() could look at the widget tree, so
//                         this is measured with an event filter DURING the
//                         build rather than asserted afterwards.
//   compact     = "-1"    1 builds the strip in COMPACT mode -- the MIXER's
//                         mount (proposal 48 D5a), where the row stacks and
//                         the name shrinks (QBX-201). -1 (the default) leaves
//                         it as constructed, which is the Track Detail dock's
//                         full row, so every committed assertion still reads
//                         the mount it was written for.
//   maxRowWidth = "-1"    if >= 0, EVERY row's layout minimum width must be at
//                         most this. A FLOOR-shaped claim, and the one number
//                         that decides whether an insert list can live in a
//                         mixer column at all: proposal 48 measured the full
//                         row at 113 px against a WIDE strip of 96, which is
//                         what compact mode exists for. Asserted over every
//                         row rather than the addressed one, because a row's
//                         width is set by its longest content and the point is
//                         that NONE of them pushes the column open.
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
    int     strayWindows_ = -1;
    int     compact_      = -1;
    int     maxRowWidth_  = -1;
};

// plugin-row-command — drive an insert ROW the way a user does (QBX-201): its
// context-menu items, and the double-click on the row itself.
//
// WHY A GESTURE VERB AND NOT A MODEL ONE. Every command here already has a
// verb that performs it directly (`remove-plugin`, `set-plugin-bypass`); what
// those cannot say is that the MENU ITEM and the DOUBLE-CLICK reach them. The
// menu items call SPluginEffectStrip::runRowCommand() and nothing else, and
// `gesture="double-click"` sends a REAL MouseButtonDblClick to the row's
// container so the strip's own event filter is what dispatches it — delete the
// installEventFilter() call and this stops opening anything. Same discipline
// as SMixerStrip::driveControl and SSendStrip's own verb.
//
// `gesture="menu"` goes through SPluginEffectStrip::triggerRowMenuItem(),
// which builds the menu the right-click builds, WIRES IT THE SAME WAY and
// triggers the named item. So the QAction -> command connection is on the path
// too, and a greyed item refuses exactly as a user's click would. What stays
// out of reach from here is QMenu::exec() and the event's accept() — a modal
// in a headless run is a hang, which is the same limit proposal 48 AC4.2's
// `runMenuCommand` lives with for the mixer strip's own menu.
//
// NOTHING IS SHOWN. `edit` opens the plugin window through the production
// path, which puts nothing on screen under --test-case (see
// SPluginEffectStrip::openParamEditor) while still building and attaching the
// editor — so the proof is the editor REGISTRY, read with
// `plugin-generic-editor action="assert"` / `plugin-native-editor
// action="assert"` after this verb.
//
//   trackPath  = ""        index-path from the root mixer; empty = trackIndex
//   trackIndex = "0"
//   slotIndex  = "0"       which row
//   command    = ""        `edit` | `bypass` | `reload` | `remove`. Required
//                          for gesture="menu"; ignored for "double-click",
//                          which has only one meaning.
//   gesture    = "menu"    `menu` runs the item's own code path;
//                          `double-click` delivers the real event to the row
//   compact    = "1"       build the strip the way the MIXER mounts it, which
//                          is where these affordances are the only ones there
//                          are. "0" is the Track Detail dock's full row.
//   expect     = "1"       what the seam must return: 1 = the row offered the
//                          command, 0 = it refused. A refusal is a real claim
//                          — `edit` and `bypass` on a Missing slot have
//                          nothing to act on, and a verb that reported success
//                          there would hide the greying.
class SPluginRowCommandAction : public SAction {
public:
    QString name() const override
    {
        return QStringLiteral( "plugin-row-command" );
    }
    QStringList knownAttributes() const override
    {
        return { "trackPath", "trackIndex", "slotIndex", "command",
                 "gesture", "compact", "expect" };
    }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString trackPath_;
    int     trackIndex_ = 0;
    int     slotIndex_  = 0;
    QString command_;
    QString gesture_    = QStringLiteral( "menu" );
    int     compact_    = 1;
    int     expect_     = 1;
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
//   action     = "open"  "open" | "open-via-strip" | "close" | "assert" |
//                        "restore"
//                        `assert` touches nothing and only checks expectOpen;
//                        `restore` drives the D2 post-load walk, which is a
//                        NO-OP under --test-case, so `expectOpen="0"` after it
//                        is what asserts that guard held.
//                        `open-via-strip` is the headless repro for the
//                        2026-08-23 fix: it opens through a THROWAWAY
//                        SPluginEffectStrip, exactly as
//                        SPluginEffectStrip::openParamEditor() does, then
//                        destroys that strip the way
//                        STrackDetailPanel::rebuildUI() destroys the real one
//                        on every track switch. `expectOpen="1"` afterwards is
//                        the assertion that the editor's lifetime no longer
//                        depends on the strip.
//   expectOpen = "1"     what isOpenFor() must say afterwards
//   minContainerAtLeast = ""  if set, "WxH": the open editor's native container
//                        must refuse to be squeezed below W and H logical units
//                        in either extent. Empty = do not check.
//
//                        A FLOOR, NEVER AN EQUALITY, and the reason is the same
//                        one that makes hostSizeFor() platform-split: the
//                        minimum is derived from the plugin's own floor in
//                        PLUGIN units (tw.test.clap.gui rounds any proposed size
//                        down to a multiple of 16 with a floor of 16) and then
//                        converted, and that conversion DIVIDES by the device
//                        pixel ratio on Windows and X11 while being the identity
//                        on macOS. So the same correct code answers 16x16 on a
//                        Mac and 8x8 on a 2x Windows display, and an equality
//                        here would be a Mac-only assertion wearing a general
//                        one's clothes.
//
//                        What the floor DOES pin is the whole of QBX-119: the
//                        minimum used to be 0x0, so the user could drag the
//                        window down until resizeEvent() offered the plugin a
//                        size of 0x0, and an iPlug2/Skia editor dies on that
//                        (a CAMetalLayer drawable of 0x0, nil from
//                        nextDrawable, a null SkCanvas). "1x1 or more" fails on
//                        0x0 and passes on every honest answer at every ratio.
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
    QString minContainerAtLeast_;
};

// plugin-generic-editor -- the plugin-native-editor twin for the GENERIC
// slider-list editor (SPluginParamEditor, opened when a plugin has no native
// GUI or its native editor cannot embed). Added 2026-08-23 for the same
// reason plugin-native-editor's own "open-via-strip" mode exists: the
// generic editor's dialog used to be owned by SPluginEffectStrip::editors_,
// a STRIP MEMBER, so STrackDetailPanel::rebuildUI()'s `delete pluginStrip_`
// on every track switch destroyed it exactly as it destroyed the native
// editor -- and this is the window a Linux/VST3 user actually sees, because
// SPluginNativeEditor::attachPlugin() refuses a VST3 editor on X11
// (needsRunLoop), so openParamEditor() falls through to the generic editor
// for every VST3 instrument on this platform.
//
//   trackPath  = ""      index-path; empty = use trackIndex
//   trackIndex = "0"
//   slotIndex  = "0"
//   action     = "open-via-strip"  the only interesting mode: opens through a
//                        THROWAWAY SPluginEffectStrip parented to the REAL
//                        main window (a parentless strip's own window() is
//                        itself, which would silently retest the pre-fix
//                        shape -- see plugin_native_editor_survives_strip
//                        .qxa's identical caveat), calling
//                        SPluginEffectStrip::ensureGenericEditorForTest(),
//                        then destroys ONLY the strip.
//                        "close" | "assert" mirror plugin-native-editor.
//   expectOpen = "1"     what SPluginEffectStrip::isGenericEditorOpenFor()
//                        must say afterwards
class SPluginGenericEditorAction : public SAction {
public:
    QString name() const override
    {
        return QStringLiteral( "plugin-generic-editor" );
    }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString trackPath_;
    int     trackIndex_ = 0;
    int     slotIndex_  = 0;
    QString action_     = QStringLiteral( "open-via-strip" );
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
    // gesture="drag" (the default) moves the slider to `value`; "double-click"
    // sends a real MouseButtonDblClick to it instead and ignores `value`, so a
    // case can gate the RESET gesture (sdefaultreset::onDoubleClick -> the
    // plugin's own declared default) through the production filter.
    QString       gesture_    = QStringLiteral( "drag" );
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

// assert-plugin-browser — build the REAL plugin browser off screen and assert
// on what it shows (QBX-78). Same house pattern as assert-plugin-strip above
// and assert-media-options: the production widget, constructed and thrown away,
// matched against its own describe() line.
//
// WHY THE LIST IS SUPPLIED. The browser reads twPluginRegistry, which holds
// what is INSTALLED ON THIS MACHINE — two formats on the dev box, one on a CI
// runner, and whatever a developer happens to have in ~/.vst3. A format filter
// cannot be told apart from a no-op unless at least two formats are present, so
// a case asserting over the real registry would be vacuous exactly where the
// claim lives. `plugins=` hands the dialog a literal list instead, replacing
// its one registry read and nothing else (SPluginBrowserDialog::
// setPluginsForTest). Leave it out and the real registry is used, which is how
// a case checks that the combo is built from the registry at all — the two `tw`
// built-ins are present in every build.
//
// THE REMEMBERED FORMAT IS A REAL USER PREFERENCE (SOpt::PluginBrowserFormat),
// not a fixture key, so this verb SAVES AND RESTORES it around its own work,
// including on every early return. A case must not be able to leave a
// developer's browser filtered to a format they never chose, and that is also
// why the persistence claim is made HERE, inside one apply(), rather than with
// settings-remove in the case (whose own header forbids removing a key the
// case does not own).
//   plugins      = ""      literal list, `name:format:vendor:nIn/nOut:kind`
//                          per ';', kind being `fx` or `instrument`; empty =
//                          the real registry
//   kind         = "all"   which Kind the browser is opened on: `all`,
//                          `instruments` or `effects` (the ctor argument the
//                          FX strip's two buttons pass)
//   filter       = ""      typed into the REAL Filter box, so the text filter
//                          and the format filter compose the way they do for a
//                          user
//   format       = ""      ABSENT means "do not touch the combo", which is what
//                          the persistence check needs; `all` selects All, any
//                          other value selects that raw format
//   expectSelect = "1"     whether selecting `format` must SUCCEED. "0" gates
//                          the other branch: a format this machine has nothing
//                          in is not offered, the browser stays on All, and the
//                          stored preference is left alone
//   reopen       = "0"     "1" destroys the dialog after applying `format` and
//                          builds a SECOND one the same way, touching no combo.
//                          `contains`/`absent` then describe THAT dialog — the
//                          round trip through the settings file
//   reopenPlugins = ""     the list the REOPENED dialog is built from; empty =
//                          the same `plugins`. A different one is how the
//                          "this machine has nothing in the stored format"
//                          branch is reached inside a single verb, which it
//                          has to be: each verb restores the user's real
//                          preference when it returns, so no claim about the
//                          stored value can span two of them
//   stored       = ""      ABSENT = not checked; otherwise the value
//                          SOpt::PluginBrowserFormat holds AFTER the dialogs
//                          were built (`all` = empty). This is how the other
//                          half of the preference is gated: a machine whose
//                          list has nothing in the stored format shows All and
//                          must leave the stored value ALONE, which no
//                          describe() line can show
//   contains     = ""      substring that must appear in
//                          SPluginBrowserDialog::describeBrowser()
//   absent       = ""      substring that must NOT appear there
class SAssertPluginBrowserAction : public SAction {
public:
    QString name() const override
    {
        return QStringLiteral( "assert-plugin-browser" );
    }
    QStringList knownAttributes() const override
    {
        return { "plugins", "kind", "filter", "format", "expectSelect",
                 "reopen", "reopenPlugins", "stored", "contains", "absent" };
    }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString plugins_;
    QString kind_   = QStringLiteral( "all" );
    QString filter_;
    QString format_;
    bool    hasFormat_    = false;   // the attribute was PRESENT
    int     expectSelect_ = 1;
    int     reopen_       = 0;
    QString reopenPlugins_;
    QString stored_;
    bool    hasStored_    = false;
    QString contains_;
    QString absent_;
};

#endif  // SPLUGINUITESTACTIONS_H
