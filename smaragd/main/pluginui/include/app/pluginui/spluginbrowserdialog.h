#ifndef SPLUGINBROWSERDIALOG_H
#define SPLUGINBROWSERDIALOG_H

#include <QDialog>
#include <QString>
#include <memory>
#include <vector>

// Complete type needed: the unique_ptr member's deleter is instantiated in
// every TU including this header (e.g. the moc jumbo file).
#include "tw/plugins/twplugindescriptor.h"

class QComboBox;
class QLabel;
class QLineEdit;
class QTreeWidget;
class QTreeWidgetItem;

// Modal dialog for browsing and selecting a plugin to insert.
//
// A tree rather than a list since proposal 08 M2: with a real scanner there can
// be plugins of several formats with the same name, so the format is a column
// and the selection is resolved by (format, uid), never by name. The list
// repopulates when a background scan finishes — it used to snapshot the registry
// once at construction, which showed an empty browser during the startup scan.
//
// Three filters, and they compose: the Filter text box (every column), the Kind
// combo (proposal 37 §6.1) and the Format combo (QBX-78), whose choice is
// REMEMBERED because it is a preference rather than a view state. The Format
// filter HIDES rows and nothing more — it does not fold one product's formats
// into a single row and has no say in what a saved project resolves to, both of
// which need cross-format identity (plan/proposed/54, §4 tiers T3/T4 and §8
// Q2). CONTRACT.md has the whole rule, including the one writer of the stored
// value and why a machine that cannot offer the stored format must not rewrite it.
class SPluginBrowserDialog : public QDialog {
    Q_OBJECT
public:
    // What the browser is being opened FOR (proposal 37 6.1). The FX strip's
    // "Add Instrument" opens it on Instruments, "+ Add Effect" on Effects; the
    // user can still widen it to All from the combo, because refusing to SHOW a
    // plugin is not the same as refusing to insert it (the insert rules live in
    // the action, design D3).
    enum class Kind { All = 0, Instruments = 1, Effects = 2 };

    SPluginBrowserDialog(QWidget *parent = nullptr, Kind kind = Kind::All);

    // Returns the selected plugin descriptor, or null if cancelled
    const audio::twPluginDescriptor *selectedPlugin() const;

    // The FORMAT filter (QBX-78), as the raw format string the descriptors
    // carry ("clap", "vst3", "au", "tw") -- EMPTY means "All". The combo shows
    // a readable label per format and keeps the raw string as item data,
    // because the raw string is what a descriptor, the Format column and the
    // remembered setting all speak.
    QString currentFormat() const;
    // Select a format by its raw string ("" = All, which always exists).
    // Returns false when the combo has no such entry -- i.e. nothing in the
    // list has that format -- which is how the remembered value is restored
    // WITHOUT inventing a filter whose only result could be an empty browser.
    // The rebuild and the testkit verb both go through this one.
    bool    selectFormat(const QString &format);

    // What the browser currently shows, for the headless gate. One line:
    //   formats=<raw,…>|format=<raw>|kind=<all|instruments|effects>|
    //   filter=<text>|rows=<n>|visible=<n>|visibleFormats=<raw,…>|status=<text>
    // `formats` is the combo's own content (minus All) and `visibleFormats` the
    // set over the rows that are NOT hidden -- the two answers a filter can get
    // wrong in opposite directions.
    QString describeBrowser() const;

    // Populate from a literal spec instead of the registry, for the headless
    // gate. One plugin per ';' as `name:format:vendor:nIn/nOut:kind`, kind
    // being `fx` or `instrument`:
    //   "Mangrove:clap:Nassau:2/2:fx;Mangrove:vst3:Nassau:2/2:fx"
    //
    // WHAT THIS DOES AND DOES NOT REPLACE. It replaces the ONE registry read
    // in populatePlugins() and nothing else: the rows, the combo, the filter
    // and the status line are built by the production code from whatever list
    // it is handed. It exists because the real registry holds whatever is
    // INSTALLED ON THE MACHINE -- two formats on the dev box, one on a CI
    // runner -- so a case that asserted over it would be vacuous exactly where
    // it matters, and a format filter cannot be told from a no-op unless at
    // least two formats are present. The uid is synthesised as
    // `<format>.<name>` and nothing instantiates these descriptors.
    // That the list comes from the registry at all is NOT gated here; it is
    // one unchanged line, and `formats=` over a real registry (the two `tw`
    // built-ins are always there) is what the case checks it against.
    void    setPluginsForTest(const QString &spec);

protected slots:
    void onSearchTextChanged(const QString &text);
    void onKindFilterChanged(int index);
    // Persists the choice (SSettings, SOpt::PluginBrowserFormat) and refilters.
    void onFormatFilterChanged(int index);
    void onPluginActivated(QTreeWidgetItem *item, int column);
    // A background scan finished: refresh, keeping the filter and the selection.
    void onPluginScanFinished();

private:
    void populatePlugins();
    // Rebuild the Format combo over the formats `plugins` actually carries,
    // keeping the current choice. The remembered format is kept as an entry
    // even when nothing on this machine has it: a scan that has not finished
    // yet (the browser can open during the startup scan) would otherwise drop
    // the user's preference on the floor the first time it repopulates.
    void rebuildFormatCombo(const std::vector<audio::twPluginDescriptor> &plugins);
    void filterPlugins(const QString &searchText);
    void takeSelection();          // resolve the current row into selectedDescriptor_

    QLineEdit   *searchEdit_;
    QComboBox   *kindCombo_;
    QComboBox   *formatCombo_;
    // The format this dialog WANTS to be filtered to: the remembered value at
    // construction, then whatever the user picks. Distinct from the combo's
    // current entry, which can only hold a format the list has -- that is what
    // lets a rebuild recover the choice once a scan finds it, and what keeps a
    // machine without that format from quietly rewriting the preference.
    QString      wantedFormat_;
    QTreeWidget *pluginList_;
    QLabel      *statusLabel_;
    std::unique_ptr<audio::twPluginDescriptor> selectedDescriptor_;
    // Empty unless setPluginsForTest() was called; see its comment.
    std::vector<audio::twPluginDescriptor> testPlugins_;
};

#endif
