#include "spluginbrowserdialog.h"
#include "plugins/twplugindescriptor.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPushButton>
#include <QLabel>

SPluginBrowserDialog::SPluginBrowserDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle("Insert Plugin");
    setMinimumSize(400, 300);

    QVBoxLayout *mainLayout = new QVBoxLayout(this);

    // Search section
    QHBoxLayout *searchLayout = new QHBoxLayout();
    searchLayout->addWidget(new QLabel("Search:"));
    searchEdit_ = new QLineEdit();
    searchLayout->addWidget(searchEdit_);
    mainLayout->addLayout(searchLayout);

    // Plugin list
    pluginList_ = new QListWidget();
    pluginList_->setAlternatingRowColors(true);
    mainLayout->addWidget(pluginList_);

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
    connect(pluginList_, &QListWidget::itemDoubleClicked, this, &SPluginBrowserDialog::onPluginDoubleClicked);
    connect(okBtn, &QPushButton::clicked, this, &QDialog::accept);
    connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
}

const audio::twPluginDescriptor *SPluginBrowserDialog::selectedPlugin() const
{
    return selectedDescriptor_.get();
}

void SPluginBrowserDialog::populatePlugins()
{
    auto &registry = audio::pluginRegistry();
    const auto &plugins = registry.plugins();

    for (const auto &desc : plugins) {
        QString itemText = QString::fromStdString(desc.name) + " (" +
                          QString::fromStdString(desc.vendor) + ")";
        QListWidgetItem *item = new QListWidgetItem(itemText);
        item->setData(Qt::UserRole, QString::fromStdString(desc.uid));
        pluginList_->addItem(item);
    }
}

void SPluginBrowserDialog::filterPlugins(const QString &searchText)
{
    for (int i = 0; i < pluginList_->count(); ++i) {
        QListWidgetItem *item = pluginList_->item(i);
        bool matches = item->text().contains(searchText, Qt::CaseInsensitive);
        item->setHidden(!matches);
    }
}

void SPluginBrowserDialog::onSearchTextChanged(const QString &text)
{
    filterPlugins(text);
}

void SPluginBrowserDialog::onPluginDoubleClicked(QListWidgetItem *item)
{
    // Find the descriptor for this item
    QString uid = item->data(Qt::UserRole).toString();
    auto &registry = audio::pluginRegistry();
    const auto &plugins = registry.plugins();

    for (const auto &desc : plugins) {
        if (QString::fromStdString(desc.uid) == uid) {
            selectedDescriptor_ = std::make_unique<audio::twPluginDescriptor>(desc);
            accept();
            return;
        }
    }
}
