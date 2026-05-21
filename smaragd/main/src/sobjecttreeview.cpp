
#include <stdlib.h>
#include <stdio.h>

#include "sobjecttreeview.h"
#include "sobject.h"

void SObjectTreeView::objectAdded( SObject &/*parent*/, SObject &/*newChild*/ )
{
    // First look, if we know the parent.
    // If we don't know it, we don't need to add it.
//    if( 
}

void SObjectTreeView::objectRemoved( SObject &parent, SObject *oldChild )
{
    (void) parent; (void) oldChild;
}

void SObjectTreeView::objectDestroyed( QObject */*obj*/ )
{
}

SObjectTreeView::~SObjectTreeView()
{
}

SObjectTreeView::SObjectTreeView( QWidget */*parent*/, SObject *root )
    : root_( root )
{
}

