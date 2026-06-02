/****************************************************************************
** Meta object code from reading C++ file 'strack.h'
**
** Created by: The Qt Meta Object Compiler version 69 (Qt 6.11.1)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../../../main/include/strack.h"
#include <QtCore/qmetatype.h>

#include <QtCore/qtmochelpers.h>

#include <memory>


#include <QtCore/qxptype_traits.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'strack.h' doesn't include <QObject>."
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
struct qt_meta_tag_ZN6STrackE_t {};
} // unnamed namespace

template <> constexpr inline auto STrack::qt_create_metaobjectdata<qt_meta_tag_ZN6STrackE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "STrack",
        "nChannelsChanged",
        "",
        "n",
        "setNBusses",
        "trackChildWasAdded",
        "SLink&",
        "trackChildWasRemoved",
        "trackChildWasMoved",
        "offset_t",
        "newTime",
        "trackChildDurationChanged",
        "length_t",
        "newLength"
    };

    QtMocHelpers::UintData qt_methods {
        // Signal 'nChannelsChanged'
        QtMocHelpers::SignalData<void(int)>(1, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Int, 3 },
        }}),
        // Slot 'setNBusses'
        QtMocHelpers::SlotData<void(int)>(4, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Int, 3 },
        }}),
        // Slot 'trackChildWasAdded'
        QtMocHelpers::SlotData<void(SLink &)>(5, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { 0x80000000 | 6, 2 },
        }}),
        // Slot 'trackChildWasRemoved'
        QtMocHelpers::SlotData<void(SLink &)>(7, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { 0x80000000 | 6, 2 },
        }}),
        // Slot 'trackChildWasMoved'
        QtMocHelpers::SlotData<void(offset_t)>(8, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { 0x80000000 | 9, 10 },
        }}),
        // Slot 'trackChildDurationChanged'
        QtMocHelpers::SlotData<void(length_t)>(11, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { 0x80000000 | 12, 13 },
        }}),
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<STrack, qt_meta_tag_ZN6STrackE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject STrack::staticMetaObject = { {
    QMetaObject::SuperData::link<SObject::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN6STrackE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN6STrackE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN6STrackE_t>.metaTypes,
    nullptr
} };

void STrack::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<STrack *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->nChannelsChanged((*reinterpret_cast<std::add_pointer_t<int>>(_a[1]))); break;
        case 1: _t->setNBusses((*reinterpret_cast<std::add_pointer_t<int>>(_a[1]))); break;
        case 2: _t->trackChildWasAdded((*reinterpret_cast<std::add_pointer_t<SLink&>>(_a[1]))); break;
        case 3: _t->trackChildWasRemoved((*reinterpret_cast<std::add_pointer_t<SLink&>>(_a[1]))); break;
        case 4: _t->trackChildWasMoved((*reinterpret_cast<std::add_pointer_t<offset_t>>(_a[1]))); break;
        case 5: _t->trackChildDurationChanged((*reinterpret_cast<std::add_pointer_t<length_t>>(_a[1]))); break;
        default: ;
        }
    }
    if (_c == QMetaObject::IndexOfMethod) {
        if (QtMocHelpers::indexOfMethod<void (STrack::*)(int )>(_a, &STrack::nChannelsChanged, 0))
            return;
    }
}

const QMetaObject *STrack::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *STrack::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN6STrackE_t>.strings))
        return static_cast<void*>(this);
    return SObject::qt_metacast(_clname);
}

int STrack::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = SObject::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 6)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 6;
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 6)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 6;
    }
    return _id;
}

// SIGNAL 0
void STrack::nChannelsChanged(int _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 0, nullptr, _t1);
}
QT_WARNING_POP
