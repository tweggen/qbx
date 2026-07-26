#include "app/pluginui/spluginbrowserdialog.h"
#include "app/shell/sapplication.h"
#include "tw/plugins/twplugindescriptor.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QStringList>
#include <QTreeWidget>
#include <QTreeWidgetItem>

namespace {

// Item data roles: a plugin is identified by (format, uid), never by its
// display name — a scanner can legitimately find the same name twice.
constexpr int kUidRole    = Qt::UserRole;
constexpr int kFormatRole = Qt::UserRole + 1;

}  // namespace

SPluginBrowserDialog::SPluginBrowserDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle("Insert Plugin");
    setMinimumSize(520, 340);

    QVBoxLayout *mainLayout = new QVBoxLayout(this);

    // Filter section
    QHBoxLayout *searchLayout = new QHBoxLayout();
    searchLayout->addWidget(new QLabel("Filter:"));
    searchEdit_ = new QLineEdit();
    searchEdit_->setPlaceholderText("name, vendor or format");
    searchEdit_->setClearButtonEnabled(true);
    searchLayout->addWidget(searchEdit_);
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

    // Populate initial list
    populatePlugins();

    // Connect signals
    connect(searchEdit_, &QLineEdit::textChanged, this, &SPluginBrowserDialog::onSearchTextChanged);
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
    const std::vector<audio::twPluginDescriptor> plugins =
        audio::pluginRegistry().plugins();

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
    }
    for (int c = 1; c < pluginList_->columnCount(); ++c)
        pluginList_->resizeColumnToContents(c);

    statusLabel_->setText(QString("%1 plugin(s)").arg((int) plugins.size()));
    filterPlugins(searchEdit_->text());
}

void SPluginBrowserDialog::filterPlugins(const QString &searchText)
{
    int shown = 0;
    for (int i = 0; i < pluginList_->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = pluginList_->topLevelItem(i);
        bool matches = searchText.isEmpty();
        for (int c = 0; !matches && c < pluginList_->columnCount(); ++c)
            matches = item->text(c).contains(searchText, Qt::CaseInsensitive);
        item->setHidden(!matches);
        if (matches) ++shown;
    }
    if (!searchText.isEmpty()) {
        statusLabel_->setText(QString("%1 of %2 plugin(s)")
                                  .arg(shown)
                                  .arg(pluginList_->topLevelItemCount()));
    } else {
        statusLabel_->setText(
            QString("%1 plugin(s)").arg(pluginList_->topLevelItemCount()));
    }
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

void SPluginBrowserDialog::onPluginActivated(QTreeWidgetItem *item, int /*column*/)
{
    pluginList_->setCurrentItem(item);
    takeSelection();
    if (selectedDescriptor_) accept();
}
