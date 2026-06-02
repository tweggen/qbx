/****************************************************************************
** Meta object code from reading C++ file 'twcomponent.h'
**
** Created by: The Qt Meta Object Compiler version 69 (Qt 6.11.1)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../../../smaragd/tw303a/include/twcomponent.h"
#include <QtCore/qmetatype.h>

#include <QtCore/qtmochelpers.h>

#include <memory>


#include <QtCore/qxptype_traits.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'twcomponent.h' doesn't include <QObject>."
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
struct qt_meta_tag_ZN7twLatchE_t {};
} // unnamed namespace

template <> constexpr inline auto twLatch::qt_create_metaobjectdata<qt_meta_tag_ZN7twLatchE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "twLatch"
    };

    QtMocHelpers::UintData qt_methods {
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<twLatch, qt_meta_tag_ZN7twLatchE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject twLatch::staticMetaObject = { {
    QMetaObject::SuperData::link<QObject::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN7twLatchE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN7twLatchE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN7twLatchE_t>.metaTypes,
    nullptr
} };

void twLatch::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<twLatch *>(_o);
    (void)_t;
    (void)_c;
    (void)_id;
    (void)_a;
}

const QMetaObject *twLatch::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *twLatch::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN7twLatchE_t>.strings))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int twLatch::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
    return _id;
}
namespace {
struct qt_meta_tag_ZN13twLatchOutputE_t {};
} // unnamed namespace

template <> constexpr inline auto twLatchOutput::qt_create_metaobjectdata<qt_meta_tag_ZN13twLatchOutputE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "twLatchOutput"
    };

    QtMocHelpers::UintData qt_methods {
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<twLatchOutput, qt_meta_tag_ZN13twLatchOutputE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject twLatchOutput::staticMetaObject = { {
    QMetaObject::SuperData::link<QObject::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN13twLatchOutputE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN13twLatchOutputE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN13twLatchOutputE_t>.metaTypes,
    nullptr
} };

void twLatchOutput::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<twLatchOutput *>(_o);
    (void)_t;
    (void)_c;
    (void)_id;
    (void)_a;
}

const QMetaObject *twLatchOutput::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *twLatchOutput::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN13twLatchOutputE_t>.strings))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int twLatchOutput::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
    return _id;
}
namespace {
struct qt_meta_tag_ZN11twComponentE_t {};
} // unnamed namespace

template <> constexpr inline auto twComponent::qt_create_metaobjectdata<qt_meta_tag_ZN11twComponentE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "twComponent",
        "inputChanged",
        "",
        "idx_t",
        "idx",
        "twLatchOutput*",
        "former",
        "recent",
        "outputChanged",
        "renegotiationRequired",
        "twComponent*",
        "origin",
        "formatChanged",
        "twFormat",
        "oldFmt",
        "newFmt"
    };

    QtMocHelpers::UintData qt_methods {
        // Signal 'inputChanged'
        QtMocHelpers::SignalData<void(idx_t, twLatchOutput *, twLatchOutput *)>(1, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 3, 4 }, { 0x80000000 | 5, 6 }, { 0x80000000 | 5, 7 },
        }}),
        // Signal 'outputChanged'
        QtMocHelpers::SignalData<void(idx_t, twLatchOutput *, twLatchOutput *)>(8, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 3, 4 }, { 0x80000000 | 5, 6 }, { 0x80000000 | 5, 7 },
        }}),
        // Signal 'renegotiationRequired'
        QtMocHelpers::SignalData<void(twComponent *)>(9, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 10, 11 },
        }}),
        // Signal 'formatChanged'
        QtMocHelpers::SignalData<void(idx_t, twFormat, twFormat)>(12, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 3, 4 }, { 0x80000000 | 13, 14 }, { 0x80000000 | 13, 15 },
        }}),
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<twComponent, qt_meta_tag_ZN11twComponentE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject twComponent::staticMetaObject = { {
    QMetaObject::SuperData::link<QObject::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN11twComponentE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN11twComponentE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN11twComponentE_t>.metaTypes,
    nullptr
} };

void twComponent::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<twComponent *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->inputChanged((*reinterpret_cast<std::add_pointer_t<idx_t>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<twLatchOutput*>>(_a[2])),(*reinterpret_cast<std::add_pointer_t<twLatchOutput*>>(_a[3]))); break;
        case 1: _t->outputChanged((*reinterpret_cast<std::add_pointer_t<idx_t>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<twLatchOutput*>>(_a[2])),(*reinterpret_cast<std::add_pointer_t<twLatchOutput*>>(_a[3]))); break;
        case 2: _t->renegotiationRequired((*reinterpret_cast<std::add_pointer_t<twComponent*>>(_a[1]))); break;
        case 3: _t->formatChanged((*reinterpret_cast<std::add_pointer_t<idx_t>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<twFormat>>(_a[2])),(*reinterpret_cast<std::add_pointer_t<twFormat>>(_a[3]))); break;
        default: ;
        }
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        switch (_id) {
        default: *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType(); break;
        case 0:
            switch (*reinterpret_cast<int*>(_a[1])) {
            default: *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType(); break;
            case 2:
            case 1:
                *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType::fromType< twLatchOutput* >(); break;
            }
            break;
        case 1:
            switch (*reinterpret_cast<int*>(_a[1])) {
            default: *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType(); break;
            case 2:
            case 1:
                *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType::fromType< twLatchOutput* >(); break;
            }
            break;
        case 2:
            switch (*reinterpret_cast<int*>(_a[1])) {
            default: *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType(); break;
            case 0:
                *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType::fromType< twComponent* >(); break;
            }
            break;
        }
    }
    if (_c == QMetaObject::IndexOfMethod) {
        if (QtMocHelpers::indexOfMethod<void (twComponent::*)(idx_t , twLatchOutput * , twLatchOutput * )>(_a, &twComponent::inputChanged, 0))
            return;
        if (QtMocHelpers::indexOfMethod<void (twComponent::*)(idx_t , twLatchOutput * , twLatchOutput * )>(_a, &twComponent::outputChanged, 1))
            return;
        if (QtMocHelpers::indexOfMethod<void (twComponent::*)(twComponent * )>(_a, &twComponent::renegotiationRequired, 2))
            return;
        if (QtMocHelpers::indexOfMethod<void (twComponent::*)(idx_t , twFormat , twFormat )>(_a, &twComponent::formatChanged, 3))
            return;
    }
}

const QMetaObject *twComponent::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *twComponent::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN11twComponentE_t>.strings))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int twComponent::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 4)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 4;
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 4)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 4;
    }
    return _id;
}

// SIGNAL 0
void twComponent::inputChanged(idx_t _t1, twLatchOutput * _t2, twLatchOutput * _t3)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 0, nullptr, _t1, _t2, _t3);
}

// SIGNAL 1
void twComponent::outputChanged(idx_t _t1, twLatchOutput * _t2, twLatchOutput * _t3)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 1, nullptr, _t1, _t2, _t3);
}

// SIGNAL 2
void twComponent::renegotiationRequired(twComponent * _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 2, nullptr, _t1);
}

// SIGNAL 3
void twComponent::formatChanged(idx_t _t1, twFormat _t2, twFormat _t3)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 3, nullptr, _t1, _t2, _t3);
}
QT_WARNING_POP
