/****************************************************************************
** Meta object code from reading C++ file 'scut.h'
**
** Created by: The Qt Meta Object Compiler version 69 (Qt 6.11.1)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../../../smaragd/main/include/scut.h"
#include <QtCore/qmetatype.h>

#include <QtCore/qtmochelpers.h>

#include <memory>


#include <QtCore/qxptype_traits.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'scut.h' doesn't include <QObject>."
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
struct qt_meta_tag_ZN4SCutE_t {};
} // unnamed namespace

template <> constexpr inline auto SCut::qt_create_metaobjectdata<qt_meta_tag_ZN4SCutE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "SCut",
        "setLoopStart",
        "",
        "offset_t",
        "setStartOffset",
        "setDuration",
        "length_t"
    };

    QtMocHelpers::UintData qt_methods {
        // Slot 'setLoopStart'
        QtMocHelpers::SlotData<void(offset_t)>(1, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 3, 2 },
        }}),
        // Slot 'setStartOffset'
        QtMocHelpers::SlotData<void(offset_t)>(4, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 3, 2 },
        }}),
        // Slot 'setDuration'
        QtMocHelpers::SlotData<void(length_t)>(5, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 6, 2 },
        }}),
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<SCut, qt_meta_tag_ZN4SCutE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject SCut::staticMetaObject = { {
    QMetaObject::SuperData::link<SObject::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN4SCutE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN4SCutE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN4SCutE_t>.metaTypes,
    nullptr
} };

void SCut::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<SCut *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->setLoopStart((*reinterpret_cast<std::add_pointer_t<offset_t>>(_a[1]))); break;
        case 1: _t->setStartOffset((*reinterpret_cast<std::add_pointer_t<offset_t>>(_a[1]))); break;
        case 2: _t->setDuration((*reinterpret_cast<std::add_pointer_t<length_t>>(_a[1]))); break;
        default: ;
        }
    }
}

const QMetaObject *SCut::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *SCut::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN4SCutE_t>.strings))
        return static_cast<void*>(this);
    return SObject::qt_metacast(_clname);
}

int SCut::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = SObject::qt_metacall(_c, _id, _a);
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
