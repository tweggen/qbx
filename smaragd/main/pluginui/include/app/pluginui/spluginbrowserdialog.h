#ifndef SPLUGINBROWSERDIALOG_H
#define SPLUGINBROWSERDIALOG_H

#include <QDialog>
#include <memory>

// Complete type needed: the unique_ptr member's deleter is instantiated in
// every TU including this header (e.g. the moc jumbo file).
#include "tw/plugins/twplugindescriptor.h"

class QLineEdit;
class QListWidget;
class QListWidgetItem;

// Modal dialog for browsing and selecting a plugin to insert.
// User can search by name and double-click to select.
class SPluginBrowserDialog : public QDialog {
    Q_OBJECT
public:
    SPluginBrowserDialog(QWidget *parent = nullptr);

    // Returns the selected plugin descriptor, or null if cancelled
    const audio::twPluginDescriptor *selectedPlugin() const;

protected slots:
    void onSearchTextChanged(const QString &text);
    void onPluginDoubleClicked(QListWidgetItem *item);

private:
    void populatePlugins();
    void filterPlugins(const QString &searchText);

    QLineEdit *searchEdit_;
    QListWidget *pluginList_;
    std::unique_ptr<audio::twPluginDescriptor> selectedDescriptor_;
};

#endif
