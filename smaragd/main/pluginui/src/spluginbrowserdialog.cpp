#include "app/pluginui/spluginbrowserdialog.h"
#include "app/shell/sapplication.h"
// SSettings is the route to the remembered Format choice (QBX-78). pluginui may
// not include app/servicesui (check_layering.py), which is where the option KEY
// lives, so the key is spelled ONCE in SOpt and reached through SSettings'
// typed accessor -- the same arrangement pluginSearchPaths() already has.
#include "app/shell/ssettings.h"
#include "tw/core/twlog.h"
#include "tw/plugins/twplugindescriptor.h"
#include <QVBoxLayout>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QSignalBlocker>
#include <QStringList>
#include <QTreeWidget>
#include <QTreeWidgetItem>

#include <cstdint>
#include <set>

namespace {

// Item data roles: a plugin is identified by (format, uid), never by its
// display name — a scanner can legitimately find the same name twice.
constexpr int kUidRole    = Qt::UserRole;
constexpr int kFormatRole = Qt::UserRole + 1;
// Whether the row is an INSTRUMENT, kept on the item so the Kind filter never
// has to go back to the registry (a background scan may have replaced it).
constexpr int kInstrRole  = Qt::UserRole + 2;

// The combo's own item data: the RAW format string, empty for "All".
constexpr int kComboFormatRole = Qt::UserRole;

// What a format is called in front of a user. The raw string is what the
// descriptors, the Format column and the remembered setting carry; this is only
// the label, and an unknown format falls back to the raw string upper-cased
// rather than being hidden -- a plugin the scanner found must be reachable in
// the browser whatever this function knows about its format.
QString formatLabel( const QString &raw )
{
    if( raw == QLatin1String( "clap" ) ) return QStringLiteral( "CLAP" );
    if( raw == QLatin1String( "vst3" ) ) return QStringLiteral( "VST3" );
    if( raw == QLatin1String( "au" ) )   return QStringLiteral( "Audio Unit" );
    if( raw == QLatin1String( "lv2" ) )  return QStringLiteral( "LV2" );
    // "tw" is the in-house format: the linked-in pass-through and the native
    // 303, which are present in every build and installed from nowhere.
    if( raw == QLatin1String( "tw" ) )   return QStringLiteral( "Built-in" );
    return raw.toUpper();
}

}  // namespace

SPluginBrowserDialog::SPluginBrowserDialog(QWidget *parent, Kind kind)
    : QDialog(parent)
{
    setWindowTitle(kind == Kind::Instruments ? "Add Instrument" : "Insert Plugin");
    setMinimumSize(520, 340);

    QVBoxLayout *mainLayout = new QVBoxLayout(this);

    // Filter section
    QHBoxLayout *searchLayout = new QHBoxLayout();
    searchLayout->addWidget(new QLabel("Filter:"));
    searchEdit_ = new QLineEdit();
    searchEdit_->setPlaceholderText("name, vendor or format");
    searchEdit_->setClearButtonEnabled(true);
    searchLayout->addWidget(searchEdit_);

    // The KIND filter (proposal 37 6.1). An instrument and an effect are the
    // same kind of object with a different role, so they live in one list and
    // this narrows it - rather than two browsers that would drift apart.
    searchLayout->addWidget(new QLabel("Kind:"));
    kindCombo_ = new QComboBox();
    kindCombo_->addItem("All");
    kindCombo_->addItem("Instruments");
    kindCombo_->addItem("Effects");
    kindCombo_->setCurrentIndex((int) kind);
    searchLayout->addWidget(kindCombo_);

    // The FORMAT filter (QBX-78). One product can legitimately be installed in
    // several formats -- that is why the list is a tree with a Format column
    // and why a selection is resolved by (format, uid) rather than by name --
    // and a user who has decided which of them they work in should not have to
    // re-read the column on every insert. Its entries come from the formats
    // the registry actually found, so it never offers a filter that can only
    // yield nothing, and the choice is REMEMBERED: it is a preference, not a
    // one-off narrowing of the list.
    //
    // What it is NOT: it hides rows, it does not fold a product's formats
    // together, and it has no say in what a saved project resolves to. Both of
    // those need cross-format product IDENTITY, which nothing in the codebase
    // has yet -- proposal 54 (§4 tiers T3/T4, and §8 Q2 for the preference
    // question) is where they belong.
    searchLayout->addWidget(new QLabel("Format:"));
    formatCombo_ = new QComboBox();
    formatCombo_->setToolTip(
        "Show only plugins in one format. Remembered between sessions, so the "
        "browser opens on the format you work in — the row count below the "
        "list says how many of the installed plugins that leaves.");
    searchLayout->addWidget(formatCombo_);
    mainLayout->addLayout(searchLayout);

    // Plugin list
    pluginList_ = new QTreeWidget();
    pluginList_->setColumnCount(4);
    pluginList_->setHeaderLabels(QStringList()
                                 << "Name" << "Format" << "Vendor" << "I/O");
    pluginList_->setRootIsDecorated(false);
    pluginList_->setAlternatingRowColors(true);
    pluginList_->setSortingEnabled(true);
    pluginList_->sortByColumn(0, Qt::AscendingOrder);
    pluginList_->header()->setStretchLastSection(false);
    pluginList_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    mainLayout->addWidget(pluginList_);

    statusLabel_ = new QLabel();
    mainLayout->addWidget(statusLabel_);

    // Button section
    QHBoxLayout *btnLayout = new QHBoxLayout();
    QPushButton *okBtn = new QPushButton("Insert");
    QPushButton *cancelBtn = new QPushButton("Cancel");
    btnLayout->addStretch();
    btnLayout->addWidget(okBtn);
    btnLayout->addWidget(cancelBtn);
    mainLayout->addLayout(btnLayout);

    // The remembered Format filter, read ONCE: populatePlugins() below builds
    // the combo and every later rebuild tries to restore this value, never the
    // settings file, so one dialog cannot be told two different things by two
    // reads (and a user's explicit "All" sticks for this dialog's lifetime).
    wantedFormat_ = SSettings::instance().pluginBrowserFormat();

    // Populate initial list
    populatePlugins();

    // Connect signals
    connect(searchEdit_, &QLineEdit::textChanged, this, &SPluginBrowserDialog::onSearchTextChanged);
    connect(kindCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &SPluginBrowserDialog::onKindFilterChanged);
    // Connected AFTER the first populatePlugins() above, deliberately: that
    // build restores the remembered value, and a persist on it would be a
    // write-back of what was just read. The LATER rebuilds -- a finished scan
    // repopulates -- are covered by the signal blocker in
    // rebuildFormatCombo(), which is where the restore lives for good.
    connect(formatCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &SPluginBrowserDialog::onFormatFilterChanged);
    connect(pluginList_, &QTreeWidget::itemDoubleClicked, this, &SPluginBrowserDialog::onPluginActivated);
    connect(okBtn, &QPushButton::clicked, this, [this]() {
        takeSelection();
        if (selectedDescriptor_) accept();
    });
    connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    // A scan started before this dialog opened (or triggered from the options
    // page while it is open) must not leave a stale, possibly empty, list.
    connect(&SApplication::app(), &SApplication::pluginScanFinished,
            this, &SPluginBrowserDialog::onPluginScanFinished);
}

const audio::twPluginDescriptor *SPluginBrowserDialog::selectedPlugin() const
{
    return selectedDescriptor_.get();
}

void SPluginBrowserDialog::populatePlugins()
{
    // By value: a background scan may replace the registry's list at any moment.
    // testPlugins_ is the headless gate's substitute for THIS read and for
    // nothing below it (see setPluginsForTest).
    const std::vector<audio::twPluginDescriptor> plugins =
        testPlugins_.empty() ? audio::pluginRegistry().plugins() : testPlugins_;

    pluginList_->clear();
    for (const auto &desc : plugins) {
        QTreeWidgetItem *item = new QTreeWidgetItem(pluginList_);
        item->setText(0, QString::fromStdString(desc.name));
        item->setText(1, QString::fromStdString(desc.format));
        item->setText(2, QString::fromStdString(desc.vendor));
        item->setText(3, QString("%1 / %2")
                             .arg((unsigned) desc.io.audioInputs)
                             .arg((unsigned) desc.io.audioOutputs));
        if (desc.isInstrument) {
            item->setToolTip(0, QString::fromStdString(desc.name)
                                    + " (instrument)\n"
                                    + QString::fromStdString(desc.path));
        } else if (!desc.path.empty()) {
            item->setToolTip(0, QString::fromStdString(desc.path));
        }
        item->setData(0, kUidRole,    QString::fromStdString(desc.uid));
        item->setData(0, kFormatRole, QString::fromStdString(desc.format));
        item->setData(0, kInstrRole,  desc.isInstrument);
    }
    for (int c = 1; c < pluginList_->columnCount(); ++c)
        pluginList_->resizeColumnToContents(c);

    // Before the filter runs: the combo is one of its two inputs, and a scan
    // that just finished may have brought a whole format into existence.
    rebuildFormatCombo(plugins);

    statusLabel_->setText(QString("%1 plugin(s)").arg((int) plugins.size()));
    filterPlugins(searchEdit_->text());
}

void SPluginBrowserDialog::rebuildFormatCombo(
    const std::vector<audio::twPluginDescriptor> &plugins)
{
    // ONLY the formats something in the list actually has. A format offered
    // over nothing is a filter whose only possible result is an empty browser,
    // and the entry a user would then have to find and undo.
    std::set<QString> formats;
    for (const auto &desc : plugins)
        if (!desc.format.empty())
            formats.insert(QString::fromStdString(desc.format));

    // Rebuilt with signals blocked: setCurrentIndex() would otherwise persist
    // the value it is restoring, and clear() alone fires a change to index -1.
    const QSignalBlocker blocked(formatCombo_);
    formatCombo_->clear();
    formatCombo_->addItem("All", QString());
    for (const QString &raw : formats)
        formatCombo_->addItem(formatLabel(raw), raw);

    // wantedFormat_ rather than the combo's current value, so a rebuild can
    // RECOVER the user's choice: the browser can be opened while the startup
    // scan is still running, when the registry holds the two built-ins alone
    // and no entry for `clap` exists yet. Falling back to All leaves the stored
    // preference untouched, and the next rebuild — the one the finished scan
    // triggers — selects it.
    if (!selectFormat(wantedFormat_)) formatCombo_->setCurrentIndex(0);
}

QString SPluginBrowserDialog::currentFormat() const
{
    if (!formatCombo_) return QString();
    return formatCombo_->currentData(kComboFormatRole).toString();
}

bool SPluginBrowserDialog::selectFormat(const QString &format)
{
    if (!formatCombo_) return false;
    const int at = formatCombo_->findData(format);
    if (at < 0) return false;
    formatCombo_->setCurrentIndex(at);
    return true;
}

void SPluginBrowserDialog::setPluginsForTest(const QString &spec)
{
    testPlugins_.clear();
    for (const QString &one : spec.split(';', Qt::SkipEmptyParts)) {
        const QStringList f = one.split(':');
        if (f.size() < 5) {
            // Loud, because the alternative is a case that quietly gates a
            // SHORTER list than its author wrote and still passes.
            TW_LOGW( "pluginui", "[browser] ignoring malformed test plugin "
                     "spec '%s' (want name:format:vendor:nIn/nOut:kind)",
                     one.toUtf8().constData() );
            continue;
        }
        audio::twPluginDescriptor desc;
        desc.name   = f[0].toStdString();
        desc.format = f[1].toStdString();
        desc.vendor = f[2].toStdString();
        const QStringList io = f[3].split('/');
        desc.io.audioInputs  = io.size() > 0 ? (std::uint16_t) io[0].toUShort() : 0;
        desc.io.audioOutputs = io.size() > 1 ? (std::uint16_t) io[1].toUShort() : 0;
        desc.isInstrument = ( f[4] == QLatin1String("instrument") );
        desc.uid = ( f[1] + '.' + f[0] ).toStdString();
        testPlugins_.push_back(desc);
    }
    populatePlugins();
}

void SPluginBrowserDialog::filterPlugins(const QString &searchText)
{
    const int     kind   = kindCombo_ ? kindCombo_->currentIndex() : 0;
    const QString format = currentFormat();   // empty = All
    int shown = 0;
    for (int i = 0; i < pluginList_->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = pluginList_->topLevelItem(i);
        const bool isInstr = item->data(0, kInstrRole).toBool();
        bool matches = searchText.isEmpty();
        for (int c = 0; !matches && c < pluginList_->columnCount(); ++c)
            matches = item->text(c).contains(searchText, Qt::CaseInsensitive);
        if (kind == 1 && !isInstr) matches = false;
        if (kind == 2 && isInstr)  matches = false;
        // Matched against the row's own kFormatRole, never its Format COLUMN
        // text: the column is display text a later change could relabel, while
        // the role is the raw string the registry handed over and the same one
        // the combo and the setting carry.
        if (!format.isEmpty()
            && item->data(0, kFormatRole).toString() != format) matches = false;
        item->setHidden(!matches);
        if (matches) ++shown;
    }
    if (!searchText.isEmpty() || kind != 0 || !format.isEmpty()) {
        statusLabel_->setText(QString("%1 of %2 plugin(s)")
                                  .arg(shown)
                                  .arg(pluginList_->topLevelItemCount()));
    } else {
        statusLabel_->setText(
            QString("%1 plugin(s)").arg(pluginList_->topLevelItemCount()));
    }
}

void SPluginBrowserDialog::onKindFilterChanged(int)
{
    filterPlugins(searchEdit_->text());
}

void SPluginBrowserDialog::onFormatFilterChanged(int)
{
    // Persisted on the CHANGE rather than on accept(): the browser is closed
    // with Cancel or with Escape as often as with Insert, and the format the
    // user chose to look through is still the format they want next time.
    // This is the ONLY writer -- a rebuild never persists what it restored, so
    // a machine that cannot offer the stored format does not overwrite it.
    wantedFormat_ = currentFormat();
    SSettings::instance().setPluginBrowserFormat(wantedFormat_);
    filterPlugins(searchEdit_->text());
}

void SPluginBrowserDialog::onSearchTextChanged(const QString &text)
{
    filterPlugins(text);
}

void SPluginBrowserDialog::onPluginScanFinished()
{
    populatePlugins();
}

void SPluginBrowserDialog::takeSelection()
{
    selectedDescriptor_.reset();

    QTreeWidgetItem *item = pluginList_->currentItem();
    if (!item || item->isHidden()) return;

    const QString uid    = item->data(0, kUidRole).toString();
    const QString format = item->data(0, kFormatRole).toString();

    // Resolved against the registry rather than reconstructed from the row: the
    // registry is the authority on path and channel counts.
    audio::twPluginDescriptor desc;
    if (audio::pluginRegistry().findByUid(format.toStdString(),
                                          uid.toStdString(), desc)) {
        selectedDescriptor_ = std::make_unique<audio::twPluginDescriptor>(desc);
    }
}

QString SPluginBrowserDialog::describeBrowser() const
{
    QStringList combo;
    for (int i = 0; i < formatCombo_->count(); ++i) {
        const QString raw = formatCombo_->itemData(i, kComboFormatRole).toString();
        if (!raw.isEmpty()) combo << raw;
    }

    // The set over the VISIBLE rows, which is the other half of the claim: a
    // filter that hides everything and a filter that hides nothing are both
    // wrong, and they fail in opposite directions.
    std::set<QString> visibleFormats;
    int visible = 0;
    for (int i = 0; i < pluginList_->topLevelItemCount(); ++i) {
        const QTreeWidgetItem *item = pluginList_->topLevelItem(i);
        if (item->isHidden()) continue;
        ++visible;
        visibleFormats.insert(item->data(0, kFormatRole).toString());
    }
    QStringList vis;
    for (const QString &raw : visibleFormats) vis << raw;

    const char *kindName = kindCombo_->currentIndex() == 1   ? "instruments"
                           : kindCombo_->currentIndex() == 2 ? "effects"
                                                             : "all";

    return QString("formats=%1|format=%2|kind=%3|filter=%4|rows=%5|visible=%6"
                   "|visibleFormats=%7|status=%8")
        .arg(combo.join(','))
        .arg(currentFormat())
        .arg(QLatin1String(kindName))
        .arg(searchEdit_->text())
        .arg(pluginList_->topLevelItemCount())
        .arg(visible)
        .arg(vis.join(','))
        .arg(statusLabel_->text());
}

void SPluginBrowserDialog::onPluginActivated(QTreeWidgetItem *item, int /*column*/)
{
    pluginList_->setCurrentItem(item);
    takeSelection();
    if (selectedDescriptor_) accept();
}
