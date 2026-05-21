
#ifndef _SOBJECTTREEVIEW_H
#define _SOBJECTTREEVIEW_H

#include <QTreeWidget>
#include <QHash>

class SObject;

class SObjectTreeView
    : public QTreeWidget
{
    Q_OBJECT
public:
    SObjectTreeView( QWidget *parent, SObject *root );
    virtual ~SObjectTreeView();

public slots:

protected:

protected slots:
    void objectAdded( SObject &parent, SObject &newChild );
    void objectRemoved( SObject &parent, SObject *oldChild );
    void objectDestroyed( QObject *obj );

private:
    QHash<void*,QTreeWidgetItem> listViewItemDict_;
    SObject *root_;
};

#endif
