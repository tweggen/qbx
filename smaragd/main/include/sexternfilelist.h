
#ifndef _SEXTERN_FILE_LIST_H
#define _SEXTERN_FILE_LIST_H

#include <qobject.h>
#include <QTreeWidget>
#include <qhash.h>

#include "sobject.h"
#include "sproject.h"
#include "sexternfile.h"

class SExternFileList;

class SExternFileItem
    : public QTreeWidgetItem
{
public:
    SExternFileItem( SExternFileList *parent, const SExternFile & );
    const SExternFile &getExternFile() const { return externFile_; };
private:
    const SExternFile &externFile_;
};

class SExternFileList
    : public QTreeWidget
{
    Q_OBJECT
public:
    SExternFileList( QWidget *parent, SProject &project );
    virtual ~SExternFileList();

protected:
    void startDrag(Qt::DropActions supportedActions) override;

private slots:
    void externFileRemoved( const QString );
    void externFileAdded( const SExternFile & );
    void externFileRefChanged();

    // Live assets (proposal 05 feature (b)) are listed alongside sample files.
    // An asset body is a plain SObject (not an SExternFile), so it gets its own
    // rows keyed by asset name.
    void assetAdded( const QString &name, SObject &body );
    void assetRemoved( const QString &name );
    void assetRefChanged();

private:
    SProject &project_;
    // This is ugly, but I don't know a better solution. We keep a second dict here
    // for the items, though they must also be kept inside the list...
    QHash<QString,SExternFileItem*> itemDict_;
    QHash<QString,QTreeWidgetItem*> assetItemDict_;   // asset name -> row
    QHash<SObject*,QString> assetNameByBody_;          // body -> asset name (refs)
};

#endif
