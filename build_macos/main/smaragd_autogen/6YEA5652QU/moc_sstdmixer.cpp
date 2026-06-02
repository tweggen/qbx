/****************************************************************************
** Meta object code from reading C++ file 'sstdmixer.h'
**
** Created by: The Qt Meta Object Compiler version 69 (Qt 6.11.1)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../../../smaragd/main/include/sstdmixer.h"
#include <QtCore/qmetatype.h>

#include <QtCore/qtmochelpers.h>

#include <memory>


#include <QtCore/qxptype_traits.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'sstdmixer.h' doesn't include <QObject>."
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
struct qt_meta_tag_ZN9SStdMixerE_t {};
} // unnamed namespace

template <> constexpr inline auto SStdMixer::qt_create_metaobjectdata<qt_meta_tag_ZN9SStdMixerE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "SStdMixer",
        "nBussesChanged",
        "",
        "n",
        "trackInserted",
        "newIndex",
        "STrack&",
        "pt",
        "trackRemoved",
        "oldIndex",
        "setNBusses",
        "insertTrack",
        "track",
        "removeTrack",
        "trackIndex",
        "SLink&",
        "mixerUpdateTrackRemoved",
        "mixerUpdateTrackAdded",
        "mixerChildDurationChanged",
        "length_t",
        "trackVolumeChanged"
    };

    QtMocHelpers::UintData qt_methods {
        // Signal 'nBussesChanged'
        QtMocHelpers::SignalData<void(int)>(1, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Int, 3 },
        }}),
        // Signal 'trackInserted'
        QtMocHelpers::SignalData<void(int, STrack &)>(4, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Int, 5 }, { 0x80000000 | 6, 7 },
        }}),
        // Signal 'trackRemoved'
        QtMocHelpers::SignalData<void(int, STrack &)>(8, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Int, 9 }, { 0x80000000 | 6, 7 },
        }}),
        // Slot 'setNBusses'
        QtMocHelpers::SlotData<int(int)>(10, 2, QMC::AccessPublic, QMetaType::Int, {{
            { QMetaType::Int, 3 },
        }}),
        // Slot 'insertTrack'
        QtMocHelpers::SlotData<void(int, STrack &)>(11, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Int, 5 }, { 0x80000000 | 6, 12 },
        }}),
        // Slot 'removeTrack'
        QtMocHelpers::SlotData<int(int)>(13, 2, QMC::AccessPublic, QMetaType::Int, {{
            { QMetaType::Int, 14 },
        }}),
        // Slot 'removeTrack'
        QtMocHelpers::SlotData<int(SLink &)>(13, 2, QMC::AccessPublic, QMetaType::Int, {{
            { 0x80000000 | 15, 12 },
        }}),
        // Slot 'mixerUpdateTrackRemoved'
        QtMocHelpers::SlotData<void(int, STrack &)>(16, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { QMetaType::Int, 2 }, { 0x80000000 | 6, 2 },
        }}),
        // Slot 'mixerUpdateTrackAdded'
        QtMocHelpers::SlotData<void(int, STrack &)>(17, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { QMetaType::Int, 2 }, { 0x80000000 | 6, 2 },
        }}),
        // Slot 'mixerChildDurationChanged'
        QtMocHelpers::SlotData<void(length_t)>(18, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { 0x80000000 | 19, 2 },
        }}),
        // Slot 'trackVolumeChanged'
        QtMocHelpers::SlotData<void(double)>(20, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { QMetaType::Double, 2 },
        }}),
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<SStdMixer, qt_meta_tag_ZN9SStdMixerE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject SStdMixer::staticMetaObject = { {
    QMetaObject::SuperData::link<SObject::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN9SStdMixerE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN9SStdMixerE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN9SStdMixerE_t>.metaTypes,
    nullptr
} };

void SStdMixer::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<SStdMixer *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->nBussesChanged((*reinterpret_cast<std::add_pointer_t<int>>(_a[1]))); break;
        case 1: _t->trackInserted((*reinterpret_cast<std::add_pointer_t<int>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<STrack&>>(_a[2]))); break;
        case 2: _t->trackRemoved((*reinterpret_cast<std::add_pointer_t<int>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<STrack&>>(_a[2]))); break;
        case 3: { int _r = _t->setNBusses((*reinterpret_cast<std::add_pointer_t<int>>(_a[1])));
            if (_a[0]) *reinterpret_cast<int*>(_a[0]) = std::move(_r); }  break;
        case 4: _t->insertTrack((*reinterpret_cast<std::add_pointer_t<int>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<STrack&>>(_a[2]))); break;
        case 5: { int _r = _t->removeTrack((*reinterpret_cast<std::add_pointer_t<int>>(_a[1])));
            if (_a[0]) *reinterpret_cast<int*>(_a[0]) = std::move(_r); }  break;
        case 6: { int _r = _t->removeTrack((*reinterpret_cast<std::add_pointer_t<SLink&>>(_a[1])));
            if (_a[0]) *reinterpret_cast<int*>(_a[0]) = std::move(_r); }  break;
        case 7: _t->mixerUpdateTrackRemoved((*reinterpret_cast<std::add_pointer_t<int>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<STrack&>>(_a[2]))); break;
        case 8: _t->mixerUpdateTrackAdded((*reinterpret_cast<std::add_pointer_t<int>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<STrack&>>(_a[2]))); break;
        case 9: _t->mixerChildDurationChanged((*reinterpret_cast<std::add_pointer_t<length_t>>(_a[1]))); break;
        case 10: _t->trackVolumeChanged((*reinterpret_cast<std::add_pointer_t<double>>(_a[1]))); break;
        default: ;
        }
    }
    if (_c == QMetaObject::IndexOfMethod) {
        if (QtMocHelpers::indexOfMethod<void (SStdMixer::*)(int )>(_a, &SStdMixer::nBussesChanged, 0))
            return;
        if (QtMocHelpers::indexOfMethod<void (SStdMixer::*)(int , STrack & )>(_a, &SStdMixer::trackInserted, 1))
            return;
        if (QtMocHelpers::indexOfMethod<void (SStdMixer::*)(int , STrack & )>(_a, &SStdMixer::trackRemoved, 2))
            return;
    }
}

const QMetaObject *SStdMixer::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *SStdMixer::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN9SStdMixerE_t>.strings))
        return static_cast<void*>(this);
    return SObject::qt_metacast(_clname);
}

int SStdMixer::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = SObject::qt_metacall(_c, _id, _a);
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
void SStdMixer::nBussesChanged(int _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 0, nullptr, _t1);
}

// SIGNAL 1
void SStdMixer::trackInserted(int _t1, STrack & _t2)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 1, nullptr, _t1, _t2);
}

// SIGNAL 2
void SStdMixer::trackRemoved(int _t1, STrack & _t2)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 2, nullptr, _t1, _t2);
}
QT_WARNING_POP
