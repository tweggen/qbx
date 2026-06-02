/****************************************************************************
** Meta object code from reading C++ file 'sobject.h'
**
** Created by: The Qt Meta Object Compiler version 69 (Qt 6.11.1)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../../../main/include/sobject.h"
#include <QtCore/qmetatype.h>

#include <QtCore/qtmochelpers.h>

#include <memory>


#include <QtCore/qxptype_traits.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'sobject.h' doesn't include <QObject>."
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
struct qt_meta_tag_ZN7SObjectE_t {};
} // unnamed namespace

template <> constexpr inline auto SObject::qt_create_metaobjectdata<qt_meta_tag_ZN7SObjectE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "SObject",
        "soloChanged",
        "",
        "mutedChanged",
        "volumeChanged",
        "panChanged",
        "delayChanged",
        "sNameChanged",
        "durationChanged",
        "length_t",
        "newDuration",
        "childObjectAdded",
        "SLink&",
        "child",
        "childObjectRemoved",
        "gotUnreferenced",
        "gotReferenced",
        "nRefsChanged",
        "setSolo",
        "setMuted",
        "setVolume",
        "setPan",
        "setDelay",
        "setSName",
        "setDuration",
        "addRef",
        "removeRef",
        "invalidatePreview",
        "Solo",
        "Muted",
        "Volume",
        "Pan",
        "Delay",
        "SName"
    };

    QtMocHelpers::UintData qt_methods {
        // Signal 'soloChanged'
        QtMocHelpers::SignalData<void(bool)>(1, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Bool, 2 },
        }}),
        // Signal 'mutedChanged'
        QtMocHelpers::SignalData<void(bool)>(3, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Bool, 2 },
        }}),
        // Signal 'volumeChanged'
        QtMocHelpers::SignalData<void(double)>(4, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Double, 2 },
        }}),
        // Signal 'panChanged'
        QtMocHelpers::SignalData<void(double)>(5, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Double, 2 },
        }}),
        // Signal 'delayChanged'
        QtMocHelpers::SignalData<void(double)>(6, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Double, 2 },
        }}),
        // Signal 'sNameChanged'
        QtMocHelpers::SignalData<void(const QString &)>(7, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 2 },
        }}),
        // Signal 'durationChanged'
        QtMocHelpers::SignalData<void(length_t)>(8, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 9, 10 },
        }}),
        // Signal 'childObjectAdded'
        QtMocHelpers::SignalData<void(SLink &)>(11, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 12, 13 },
        }}),
        // Signal 'childObjectRemoved'
        QtMocHelpers::SignalData<void(SLink &)>(14, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 12, 13 },
        }}),
        // Signal 'gotUnreferenced'
        QtMocHelpers::SignalData<void()>(15, 2, QMC::AccessPublic, QMetaType::Void),
        // Signal 'gotReferenced'
        QtMocHelpers::SignalData<void()>(16, 2, QMC::AccessPublic, QMetaType::Void),
        // Signal 'nRefsChanged'
        QtMocHelpers::SignalData<void()>(17, 2, QMC::AccessPublic, QMetaType::Void),
        // Slot 'setSolo'
        QtMocHelpers::SlotData<void(bool)>(18, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Bool, 2 },
        }}),
        // Slot 'setMuted'
        QtMocHelpers::SlotData<void(bool)>(19, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Bool, 2 },
        }}),
        // Slot 'setVolume'
        QtMocHelpers::SlotData<void(double)>(20, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Double, 2 },
        }}),
        // Slot 'setPan'
        QtMocHelpers::SlotData<void(double)>(21, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Double, 2 },
        }}),
        // Slot 'setDelay'
        QtMocHelpers::SlotData<void(double)>(22, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Double, 2 },
        }}),
        // Slot 'setSName'
        QtMocHelpers::SlotData<void(const QString &)>(23, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 2 },
        }}),
        // Slot 'setDuration'
        QtMocHelpers::SlotData<void(length_t)>(24, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 9, 2 },
        }}),
        // Slot 'addRef'
        QtMocHelpers::SlotData<void()>(25, 2, QMC::AccessPublic, QMetaType::Void),
        // Slot 'removeRef'
        QtMocHelpers::SlotData<void()>(26, 2, QMC::AccessPublic, QMetaType::Void),
        // Slot 'invalidatePreview'
        QtMocHelpers::SlotData<void()>(27, 2, QMC::AccessPublic, QMetaType::Void),
    };
    QtMocHelpers::UintData qt_properties {
        // property 'Solo'
        QtMocHelpers::PropertyData<bool>(28, QMetaType::Bool, QMC::DefaultPropertyFlags | QMC::Writable | QMC::StdCppSet),
        // property 'Muted'
        QtMocHelpers::PropertyData<bool>(29, QMetaType::Bool, QMC::DefaultPropertyFlags | QMC::Writable | QMC::StdCppSet),
        // property 'Volume'
        QtMocHelpers::PropertyData<double>(30, QMetaType::Double, QMC::DefaultPropertyFlags | QMC::Writable | QMC::StdCppSet),
        // property 'Pan'
        QtMocHelpers::PropertyData<double>(31, QMetaType::Double, QMC::DefaultPropertyFlags | QMC::Writable | QMC::StdCppSet),
        // property 'Delay'
        QtMocHelpers::PropertyData<double>(32, QMetaType::Double, QMC::DefaultPropertyFlags | QMC::Writable | QMC::StdCppSet),
        // property 'SName'
        QtMocHelpers::PropertyData<QString>(33, QMetaType::QString, QMC::DefaultPropertyFlags | QMC::Writable | QMC::StdCppSet),
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<SObject, qt_meta_tag_ZN7SObjectE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject SObject::staticMetaObject = { {
    QMetaObject::SuperData::link<QObject::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN7SObjectE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN7SObjectE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN7SObjectE_t>.metaTypes,
    nullptr
} };

void SObject::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<SObject *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->soloChanged((*reinterpret_cast<std::add_pointer_t<bool>>(_a[1]))); break;
        case 1: _t->mutedChanged((*reinterpret_cast<std::add_pointer_t<bool>>(_a[1]))); break;
        case 2: _t->volumeChanged((*reinterpret_cast<std::add_pointer_t<double>>(_a[1]))); break;
        case 3: _t->panChanged((*reinterpret_cast<std::add_pointer_t<double>>(_a[1]))); break;
        case 4: _t->delayChanged((*reinterpret_cast<std::add_pointer_t<double>>(_a[1]))); break;
        case 5: _t->sNameChanged((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1]))); break;
        case 6: _t->durationChanged((*reinterpret_cast<std::add_pointer_t<length_t>>(_a[1]))); break;
        case 7: _t->childObjectAdded((*reinterpret_cast<std::add_pointer_t<SLink&>>(_a[1]))); break;
        case 8: _t->childObjectRemoved((*reinterpret_cast<std::add_pointer_t<SLink&>>(_a[1]))); break;
        case 9: _t->gotUnreferenced(); break;
        case 10: _t->gotReferenced(); break;
        case 11: _t->nRefsChanged(); break;
        case 12: _t->setSolo((*reinterpret_cast<std::add_pointer_t<bool>>(_a[1]))); break;
        case 13: _t->setMuted((*reinterpret_cast<std::add_pointer_t<bool>>(_a[1]))); break;
        case 14: _t->setVolume((*reinterpret_cast<std::add_pointer_t<double>>(_a[1]))); break;
        case 15: _t->setPan((*reinterpret_cast<std::add_pointer_t<double>>(_a[1]))); break;
        case 16: _t->setDelay((*reinterpret_cast<std::add_pointer_t<double>>(_a[1]))); break;
        case 17: _t->setSName((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1]))); break;
        case 18: _t->setDuration((*reinterpret_cast<std::add_pointer_t<length_t>>(_a[1]))); break;
        case 19: _t->addRef(); break;
        case 20: _t->removeRef(); break;
        case 21: _t->invalidatePreview(); break;
        default: ;
        }
    }
    if (_c == QMetaObject::IndexOfMethod) {
        if (QtMocHelpers::indexOfMethod<void (SObject::*)(bool )>(_a, &SObject::soloChanged, 0))
            return;
        if (QtMocHelpers::indexOfMethod<void (SObject::*)(bool )>(_a, &SObject::mutedChanged, 1))
            return;
        if (QtMocHelpers::indexOfMethod<void (SObject::*)(double )>(_a, &SObject::volumeChanged, 2))
            return;
        if (QtMocHelpers::indexOfMethod<void (SObject::*)(double )>(_a, &SObject::panChanged, 3))
            return;
        if (QtMocHelpers::indexOfMethod<void (SObject::*)(double )>(_a, &SObject::delayChanged, 4))
            return;
        if (QtMocHelpers::indexOfMethod<void (SObject::*)(const QString & )>(_a, &SObject::sNameChanged, 5))
            return;
        if (QtMocHelpers::indexOfMethod<void (SObject::*)(length_t )>(_a, &SObject::durationChanged, 6))
            return;
        if (QtMocHelpers::indexOfMethod<void (SObject::*)(SLink & )>(_a, &SObject::childObjectAdded, 7))
            return;
        if (QtMocHelpers::indexOfMethod<void (SObject::*)(SLink & )>(_a, &SObject::childObjectRemoved, 8))
            return;
        if (QtMocHelpers::indexOfMethod<void (SObject::*)()>(_a, &SObject::gotUnreferenced, 9))
            return;
        if (QtMocHelpers::indexOfMethod<void (SObject::*)()>(_a, &SObject::gotReferenced, 10))
            return;
        if (QtMocHelpers::indexOfMethod<void (SObject::*)()>(_a, &SObject::nRefsChanged, 11))
            return;
    }
    if (_c == QMetaObject::ReadProperty) {
        void *_v = _a[0];
        switch (_id) {
        case 0: *reinterpret_cast<bool*>(_v) = _t->isSolo(); break;
        case 1: *reinterpret_cast<bool*>(_v) = _t->isMuted(); break;
        case 2: *reinterpret_cast<double*>(_v) = _t->getVolume(); break;
        case 3: *reinterpret_cast<double*>(_v) = _t->getPan(); break;
        case 4: *reinterpret_cast<double*>(_v) = _t->getDelay(); break;
        case 5: *reinterpret_cast<QString*>(_v) = _t->getSName(); break;
        default: break;
        }
    }
    if (_c == QMetaObject::WriteProperty) {
        void *_v = _a[0];
        switch (_id) {
        case 0: _t->setSolo(*reinterpret_cast<bool*>(_v)); break;
        case 1: _t->setMuted(*reinterpret_cast<bool*>(_v)); break;
        case 2: _t->setVolume(*reinterpret_cast<double*>(_v)); break;
        case 3: _t->setPan(*reinterpret_cast<double*>(_v)); break;
        case 4: _t->setDelay(*reinterpret_cast<double*>(_v)); break;
        case 5: _t->setSName(*reinterpret_cast<QString*>(_v)); break;
        default: break;
        }
    }
}

const QMetaObject *SObject::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *SObject::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN7SObjectE_t>.strings))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int SObject::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 22)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 22;
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 22)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 22;
    }
    if (_c == QMetaObject::ReadProperty || _c == QMetaObject::WriteProperty
            || _c == QMetaObject::ResetProperty || _c == QMetaObject::BindableProperty
            || _c == QMetaObject::RegisterPropertyMetaType) {
        qt_static_metacall(this, _c, _id, _a);
        _id -= 6;
    }
    return _id;
}

// SIGNAL 0
void SObject::soloChanged(bool _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 0, nullptr, _t1);
}

// SIGNAL 1
void SObject::mutedChanged(bool _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 1, nullptr, _t1);
}

// SIGNAL 2
void SObject::volumeChanged(double _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 2, nullptr, _t1);
}

// SIGNAL 3
void SObject::panChanged(double _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 3, nullptr, _t1);
}

// SIGNAL 4
void SObject::delayChanged(double _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 4, nullptr, _t1);
}

// SIGNAL 5
void SObject::sNameChanged(const QString & _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 5, nullptr, _t1);
}

// SIGNAL 6
void SObject::durationChanged(length_t _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 6, nullptr, _t1);
}

// SIGNAL 7
void SObject::childObjectAdded(SLink & _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 7, nullptr, _t1);
}

// SIGNAL 8
void SObject::childObjectRemoved(SLink & _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 8, nullptr, _t1);
}

// SIGNAL 9
void SObject::gotUnreferenced()
{
    QMetaObject::activate(this, &staticMetaObject, 9, nullptr);
}

// SIGNAL 10
void SObject::gotReferenced()
{
    QMetaObject::activate(this, &staticMetaObject, 10, nullptr);
}

// SIGNAL 11
void SObject::nRefsChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 11, nullptr);
}
QT_WARNING_POP
