#ifndef SPLUGINBROWSERDIALOG_H
#define SPLUGINBROWSERDIALOG_H

#include <QDialog>
#include <memory>

// Complete type needed: the unique_ptr member's deleter is instantiated in
// every TU including this header (e.g. the moc jumbo file).
#include "tw/plugins/twplugindescriptor.h"

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
class SPluginBrowserDialog : public QDialog {
    Q_OBJECT
public:
    SPluginBrowserDialog(QWidget *parent = nullptr);

    // Returns the selected plugin descriptor, or null if cancelled
    const audio::twPluginDescriptor *selectedPlugin() const;

protected slots:
    void onSearchTextChanged(const QString &text);
    void onPluginActivated(QTreeWidgetItem *item, int column);
    // A background scan finished: refresh, keeping the filter and the selection.
    void onPluginScanFinished();

private:
    void populatePlugins();
    void filterPlugins(const QString &searchText);
    void takeSelection();          // resolve the current row into selectedDescriptor_

    QLineEdit   *searchEdit_;
    QTreeWidget *pluginList_;
    QLabel      *statusLabel_;
    std::unique_ptr<audio::twPluginDescriptor> selectedDescriptor_;
};

#endif
