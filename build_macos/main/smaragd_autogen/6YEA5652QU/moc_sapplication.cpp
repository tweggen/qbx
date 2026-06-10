/****************************************************************************
** Meta object code from reading C++ file 'sapplication.h'
**
** Created by: The Qt Meta Object Compiler version 69 (Qt 6.11.1)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../../../smaragd/main/include/sapplication.h"
#include <QtCore/qmetatype.h>

#include <QtCore/qtmochelpers.h>

#include <memory>


#include <QtCore/qxptype_traits.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'sapplication.h' doesn't include <QObject>."
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
struct qt_meta_tag_ZN12SApplicationE_t {};
} // unnamed namespace

template <> constexpr inline auto SApplication::qt_create_metaobjectdata<qt_meta_tag_ZN12SApplicationE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "SApplication",
        "globalLocatorMoved",
        "",
        "offset_t",
        "newPos",
        "oldPos",
        "statusModeChanged",
        "mode",
        "setStatusMode",
        "setSelectedSLink",
        "SLink*",
        "addSelectedSLink",
        "clearSelection",
        "unselectSLink",
        "setGlobalLocatorPos",
        "setSpeakerMaxVal",
        "sample_t",
        "setPlaying"
    };

    QtMocHelpers::UintData qt_methods {
        // Signal 'globalLocatorMoved'
        QtMocHelpers::SignalData<void(offset_t, offset_t)>(1, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 3, 4 }, { 0x80000000 | 3, 5 },
        }}),
        // Signal 'statusModeChanged'
        QtMocHelpers::SignalData<void(const QString &)>(6, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 7 },
        }}),
        // Slot 'setStatusMode'
        QtMocHelpers::SlotData<void(const QString &)>(8, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 7 },
        }}),
        // Slot 'setSelectedSLink'
        QtMocHelpers::SlotData<void(SLink *)>(9, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 10, 2 },
        }}),
        // Slot 'addSelectedSLink'
        QtMocHelpers::SlotData<void(SLink *)>(11, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 10, 2 },
        }}),
        // Slot 'clearSelection'
        QtMocHelpers::SlotData<void()>(12, 2, QMC::AccessPublic, QMetaType::Void),
        // Slot 'unselectSLink'
        QtMocHelpers::SlotData<void(SLink *)>(13, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 10, 2 },
        }}),
        // Slot 'setGlobalLocatorPos'
        QtMocHelpers::SlotData<void(offset_t)>(14, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 3, 2 },
        }}),
        // Slot 'setSpeakerMaxVal'
        QtMocHelpers::SlotData<void(sample_t)>(15, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 16, 2 },
        }}),
        // Slot 'setPlaying'
        QtMocHelpers::SlotData<void(bool)>(17, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Bool, 2 },
        }}),
        // Slot 'unselectSLink'
        QtMocHelpers::SlotData<void()>(13, 2, QMC::AccessPrivate, QMetaType::Void),
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<SApplication, qt_meta_tag_ZN12SApplicationE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject SApplication::staticMetaObject = { {
    QMetaObject::SuperData::link<QApplication::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN12SApplicationE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN12SApplicationE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN12SApplicationE_t>.metaTypes,
    nullptr
} };

void SApplication::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<SApplication *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->globalLocatorMoved((*reinterpret_cast<std::add_pointer_t<offset_t>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<offset_t>>(_a[2]))); break;
        case 1: _t->statusModeChanged((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1]))); break;
        case 2: _t->setStatusMode((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1]))); break;
        case 3: _t->setSelectedSLink((*reinterpret_cast<std::add_pointer_t<SLink*>>(_a[1]))); break;
        case 4: _t->addSelectedSLink((*reinterpret_cast<std::add_pointer_t<SLink*>>(_a[1]))); break;
        case 5: _t->clearSelection(); break;
        case 6: _t->unselectSLink((*reinterpret_cast<std::add_pointer_t<SLink*>>(_a[1]))); break;
        case 7: _t->setGlobalLocatorPos((*reinterpret_cast<std::add_pointer_t<offset_t>>(_a[1]))); break;
        case 8: _t->setSpeakerMaxVal((*reinterpret_cast<std::add_pointer_t<sample_t>>(_a[1]))); break;
        case 9: _t->setPlaying((*reinterpret_cast<std::add_pointer_t<bool>>(_a[1]))); break;
        case 10: _t->unselectSLink(); break;
        default: ;
        }
    }
    if (_c == QMetaObject::IndexOfMethod) {
        if (QtMocHelpers::indexOfMethod<void (SApplication::*)(offset_t , offset_t )>(_a, &SApplication::globalLocatorMoved, 0))
            return;
        if (QtMocHelpers::indexOfMethod<void (SApplication::*)(const QString & )>(_a, &SApplication::statusModeChanged, 1))
            return;
    }
}

const QMetaObject *SApplication::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *SApplication::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN12SApplicationE_t>.strings))
        return static_cast<void*>(this);
    return QApplication::qt_metacast(_clname);
}

int SApplication::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QApplication::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 11)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 11;
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 11)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 11;
    }
    return _id;
}

// SIGNAL 0
void SApplication::globalLocatorMoved(offset_t _t1, offset_t _t2)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 0, nullptr, _t1, _t2);
}

// SIGNAL 1
void SApplication::statusModeChanged(const QString & _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 1, nullptr, _t1);
}
QT_WARNING_POP
