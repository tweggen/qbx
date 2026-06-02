/****************************************************************************
** Meta object code from reading C++ file 'sobjecttreeview.h'
**
** Created by: The Qt Meta Object Compiler version 69 (Qt 6.11.1)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../../../main/include/sobjecttreeview.h"
#include <QtCore/qmetatype.h>

#include <QtCore/qtmochelpers.h>

#include <memory>


#include <QtCore/qxptype_traits.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'sobjecttreeview.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 69
#error "This file was generated using the moc from 6.11.1. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

#ifndef Q_CONSTINIT
#define Q_CONSTINIT
#endif

QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
QT_WARNING_DISABLE_GCC("-Wuseless-cast")
namespace {
struct qt_meta_tag_ZN15SObjectTreeViewE_t {};
} // unnamed namespace

template <> constexpr inline auto SObjectTreeView::qt_create_metaobjectdata<qt_meta_tag_ZN15SObjectTreeViewE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "SObjectTreeView",
        "objectAdded",
        "",
        "SObject&",
        "parent",
        "newChild",
        "objectRemoved",
        "SObject*",
        "oldChild",
        "objectDestroyed",
        "obj"
    };

    QtMocHelpers::UintData qt_methods {
        // Slot 'objectAdded'
        QtMocHelpers::SlotData<void(SObject &, SObject &)>(1, 2, QMC::AccessProtected, QMetaType::Void, {{
            { 0x80000000 | 3, 4 }, { 0x80000000 | 3, 5 },
        }}),
        // Slot 'objectRemoved'
        QtMocHelpers::SlotData<void(SObject &, SObject *)>(6, 2, QMC::AccessProtected, QMetaType::Void, {{
            { 0x80000000 | 3, 4 }, { 0x80000000 | 7, 8 },
        }}),
        // Slot 'objectDestroyed'
        QtMocHelpers::SlotData<void(QObject *)>(9, 2, QMC::AccessProtected, QMetaType::Void, {{
            { QMetaType::QObjectStar, 10 },
        }}),
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<SObjectTreeView, qt_meta_tag_ZN15SObjectTreeViewE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject SObjectTreeView::staticMetaObject = { {
    QMetaObject::SuperData::link<QTreeWidget::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN15SObjectTreeViewE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN15SObjectTreeViewE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN15SObjectTreeViewE_t>.metaTypes,
    nullptr
} };

void SObjectTreeView::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<SObjectTreeView *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->objectAdded((*reinterpret_cast<std::add_pointer_t<SObject&>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<SObject&>>(_a[2]))); break;
        case 1: _t->objectRemoved((*reinterpret_cast<std::add_pointer_t<SObject&>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<SObject*>>(_a[2]))); break;
        case 2: _t->objectDestroyed((*reinterpret_cast<std::add_pointer_t<QObject*>>(_a[1]))); break;
        default: ;
        }
    }
}

const QMetaObject *SObjectTreeView::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *SObjectTreeView::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN15SObjectTreeViewE_t>.strings))
        return static_cast<void*>(this);
    return QTreeWidget::qt_metacast(_clname);
}

int SObjectTreeView::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QTreeWidget::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 3)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 3;
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 3)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 3;
    }
    return _id;
}
QT_WARNING_POP
