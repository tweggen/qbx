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
        // Slot 'setSelectedSLink'
        QtMocHelpers::SlotData<void(SLink *)>(6, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 7, 2 },
        }}),
        // Slot 'addSelectedSLink'
        QtMocHelpers::SlotData<void(SLink *)>(8, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 7, 2 },
        }}),
        // Slot 'clearSelection'
        QtMocHelpers::SlotData<void()>(9, 2, QMC::AccessPublic, QMetaType::Void),
        // Slot 'unselectSLink'
        QtMocHelpers::SlotData<void(SLink *)>(10, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 7, 2 },
        }}),
        // Slot 'setGlobalLocatorPos'
        QtMocHelpers::SlotData<void(offset_t)>(11, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 3, 2 },
        }}),
        // Slot 'setSpeakerMaxVal'
        QtMocHelpers::SlotData<void(sample_t)>(12, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 13, 2 },
        }}),
        // Slot 'setPlaying'
        QtMocHelpers::SlotData<void(bool)>(14, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Bool, 2 },
        }}),
        // Slot 'unselectSLink'
        QtMocHelpers::SlotData<void()>(10, 2, QMC::AccessPrivate, QMetaType::Void),
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
        case 1: _t->setSelectedSLink((*reinterpret_cast<std::add_pointer_t<SLink*>>(_a[1]))); break;
        case 2: _t->addSelectedSLink((*reinterpret_cast<std::add_pointer_t<SLink*>>(_a[1]))); break;
        case 3: _t->clearSelection(); break;
        case 4: _t->unselectSLink((*reinterpret_cast<std::add_pointer_t<SLink*>>(_a[1]))); break;
        case 5: _t->setGlobalLocatorPos((*reinterpret_cast<std::add_pointer_t<offset_t>>(_a[1]))); break;
        case 6: _t->setSpeakerMaxVal((*reinterpret_cast<std::add_pointer_t<sample_t>>(_a[1]))); break;
        case 7: _t->setPlaying((*reinterpret_cast<std::add_pointer_t<bool>>(_a[1]))); break;
        case 8: _t->unselectSLink(); break;
        default: ;
        }
    }
    if (_c == QMetaObject::IndexOfMethod) {
        if (QtMocHelpers::indexOfMethod<void (SApplication::*)(offset_t , offset_t )>(_a, &SApplication::globalLocatorMoved, 0))
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
        if (_id < 9)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 9;
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 9)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 9;
    }
    return _id;
}

// SIGNAL 0
void SApplication::globalLocatorMoved(offset_t _t1, offset_t _t2)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 0, nullptr, _t1, _t2);
}
QT_WARNING_POP
