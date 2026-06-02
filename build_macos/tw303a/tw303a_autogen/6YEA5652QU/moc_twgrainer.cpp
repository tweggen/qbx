/****************************************************************************
** Meta object code from reading C++ file 'twgrainer.h'
**
** Created by: The Qt Meta Object Compiler version 69 (Qt 6.11.1)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../../../smaragd/tw303a/include/twgrainer.h"
#include <QtCore/qmetatype.h>

#include <QtCore/qtmochelpers.h>

#include <memory>


#include <QtCore/qxptype_traits.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'twgrainer.h' doesn't include <QObject>."
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
struct qt_meta_tag_ZN9twGrainerE_t {};
} // unnamed namespace

template <> constexpr inline auto twGrainer::qt_create_metaobjectdata<qt_meta_tag_ZN9twGrainerE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "twGrainer",
        "grainSpecChanged",
        "",
        "twGrainSpec*",
        "grainSpec",
        "setSourceComponent",
        "twComponent*",
        "comp",
        "setGrainSpec",
        "setLooped",
        "setStretchFactor",
        "setPitchOffset"
    };

    QtMocHelpers::UintData qt_methods {
        // Signal 'grainSpecChanged'
        QtMocHelpers::SignalData<void(twGrainSpec *)>(1, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 3, 4 },
        }}),
        // Slot 'setSourceComponent'
        QtMocHelpers::SlotData<void(twComponent *)>(5, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 6, 7 },
        }}),
        // Slot 'setGrainSpec'
        QtMocHelpers::SlotData<void(twGrainSpec *)>(8, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 3, 4 },
        }}),
        // Slot 'setLooped'
        QtMocHelpers::SlotData<void(bool)>(9, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Bool, 2 },
        }}),
        // Slot 'setStretchFactor'
        QtMocHelpers::SlotData<void(double)>(10, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Double, 2 },
        }}),
        // Slot 'setPitchOffset'
        QtMocHelpers::SlotData<void(double)>(11, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Double, 2 },
        }}),
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<twGrainer, qt_meta_tag_ZN9twGrainerE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject twGrainer::staticMetaObject = { {
    QMetaObject::SuperData::link<twComponent::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN9twGrainerE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN9twGrainerE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN9twGrainerE_t>.metaTypes,
    nullptr
} };

void twGrainer::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<twGrainer *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->grainSpecChanged((*reinterpret_cast<std::add_pointer_t<twGrainSpec*>>(_a[1]))); break;
        case 1: _t->setSourceComponent((*reinterpret_cast<std::add_pointer_t<twComponent*>>(_a[1]))); break;
        case 2: _t->setGrainSpec((*reinterpret_cast<std::add_pointer_t<twGrainSpec*>>(_a[1]))); break;
        case 3: _t->setLooped((*reinterpret_cast<std::add_pointer_t<bool>>(_a[1]))); break;
        case 4: _t->setStretchFactor((*reinterpret_cast<std::add_pointer_t<double>>(_a[1]))); break;
        case 5: _t->setPitchOffset((*reinterpret_cast<std::add_pointer_t<double>>(_a[1]))); break;
        default: ;
        }
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        switch (_id) {
        default: *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType(); break;
        case 0:
            switch (*reinterpret_cast<int*>(_a[1])) {
            default: *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType(); break;
            case 0:
                *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType::fromType< twGrainSpec* >(); break;
            }
            break;
        case 1:
            switch (*reinterpret_cast<int*>(_a[1])) {
            default: *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType(); break;
            case 0:
                *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType::fromType< twComponent* >(); break;
            }
            break;
        case 2:
            switch (*reinterpret_cast<int*>(_a[1])) {
            default: *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType(); break;
            case 0:
                *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType::fromType< twGrainSpec* >(); break;
            }
            break;
        }
    }
    if (_c == QMetaObject::IndexOfMethod) {
        if (QtMocHelpers::indexOfMethod<void (twGrainer::*)(twGrainSpec * )>(_a, &twGrainer::grainSpecChanged, 0))
            return;
    }
}

const QMetaObject *twGrainer::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *twGrainer::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN9twGrainerE_t>.strings))
        return static_cast<void*>(this);
    return twComponent::qt_metacast(_clname);
}

int twGrainer::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = twComponent::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 6)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 6;
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 6)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 6;
    }
    return _id;
}

// SIGNAL 0
void twGrainer::grainSpecChanged(twGrainSpec * _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 0, nullptr, _t1);
}
QT_WARNING_POP
