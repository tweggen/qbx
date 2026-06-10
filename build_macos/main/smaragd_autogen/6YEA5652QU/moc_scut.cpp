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
        "windowParamsChanged",
        "",
        "setLoopStart",
        "offset_t",
        "setLoopLength",
        "length_t",
        "setStartOffset",
        "setDuration",
        "setStretch",
        "setPitchCents",
        "queueWindowParamEvent",
        "SCutWindowParamEventType",
        "type",
        "value",
        "processWindowParamEvents",
        "invalidateCapture",
        "ensureCapture",
        "twRandomSource*",
        "ensureCapturePeaks",
        "Stretch",
        "PitchCents"
    };

    QtMocHelpers::UintData qt_methods {
        // Signal 'windowParamsChanged'
        QtMocHelpers::SignalData<void()>(1, 2, QMC::AccessPublic, QMetaType::Void),
        // Slot 'setLoopStart'
        QtMocHelpers::SlotData<void(offset_t)>(3, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 4, 2 },
        }}),
        // Slot 'setLoopLength'
        QtMocHelpers::SlotData<void(length_t)>(5, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 6, 2 },
        }}),
        // Slot 'setStartOffset'
        QtMocHelpers::SlotData<void(offset_t)>(7, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 4, 2 },
        }}),
        // Slot 'setDuration'
        QtMocHelpers::SlotData<void(length_t)>(8, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 6, 2 },
        }}),
        // Slot 'setStretch'
        QtMocHelpers::SlotData<void(double)>(9, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Double, 2 },
        }}),
        // Slot 'setPitchCents'
        QtMocHelpers::SlotData<void(double)>(10, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Double, 2 },
        }}),
        // Slot 'queueWindowParamEvent'
        QtMocHelpers::SlotData<void(SCutWindowParamEventType, double)>(11, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 12, 13 }, { QMetaType::Double, 14 },
        }}),
        // Slot 'processWindowParamEvents'
        QtMocHelpers::SlotData<void()>(15, 2, QMC::AccessPublic, QMetaType::Void),
        // Slot 'invalidateCapture'
        QtMocHelpers::SlotData<void()>(16, 2, QMC::AccessPublic, QMetaType::Void),
        // Slot 'ensureCapture'
        QtMocHelpers::SlotData<twRandomSource *()>(17, 2, QMC::AccessPublic, 0x80000000 | 18),
        // Slot 'ensureCapturePeaks'
        QtMocHelpers::SlotData<bool()>(19, 2, QMC::AccessPublic, QMetaType::Bool),
    };
    QtMocHelpers::UintData qt_properties {
        // property 'Stretch'
        QtMocHelpers::PropertyData<double>(20, QMetaType::Double, QMC::DefaultPropertyFlags | QMC::Writable | QMC::StdCppSet),
        // property 'PitchCents'
        QtMocHelpers::PropertyData<double>(21, QMetaType::Double, QMC::DefaultPropertyFlags | QMC::Writable | QMC::StdCppSet),
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
        case 0: _t->windowParamsChanged(); break;
        case 1: _t->setLoopStart((*reinterpret_cast<std::add_pointer_t<offset_t>>(_a[1]))); break;
        case 2: _t->setLoopLength((*reinterpret_cast<std::add_pointer_t<length_t>>(_a[1]))); break;
        case 3: _t->setStartOffset((*reinterpret_cast<std::add_pointer_t<offset_t>>(_a[1]))); break;
        case 4: _t->setDuration((*reinterpret_cast<std::add_pointer_t<length_t>>(_a[1]))); break;
        case 5: _t->setStretch((*reinterpret_cast<std::add_pointer_t<double>>(_a[1]))); break;
        case 6: _t->setPitchCents((*reinterpret_cast<std::add_pointer_t<double>>(_a[1]))); break;
        case 7: _t->queueWindowParamEvent((*reinterpret_cast<std::add_pointer_t<SCutWindowParamEventType>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<double>>(_a[2]))); break;
        case 8: _t->processWindowParamEvents(); break;
        case 9: _t->invalidateCapture(); break;
        case 10: { twRandomSource* _r = _t->ensureCapture();
            if (_a[0]) *reinterpret_cast<twRandomSource**>(_a[0]) = std::move(_r); }  break;
        case 11: { bool _r = _t->ensureCapturePeaks();
            if (_a[0]) *reinterpret_cast<bool*>(_a[0]) = std::move(_r); }  break;
        default: ;
        }
    }
    if (_c == QMetaObject::IndexOfMethod) {
        if (QtMocHelpers::indexOfMethod<void (SCut::*)()>(_a, &SCut::windowParamsChanged, 0))
            return;
    }
    if (_c == QMetaObject::ReadProperty) {
        void *_v = _a[0];
        switch (_id) {
        case 0: *reinterpret_cast<double*>(_v) = _t->getStretch(); break;
        case 1: *reinterpret_cast<double*>(_v) = _t->getPitchCents(); break;
        default: break;
        }
    }
    if (_c == QMetaObject::WriteProperty) {
        void *_v = _a[0];
        switch (_id) {
        case 0: _t->setStretch(*reinterpret_cast<double*>(_v)); break;
        case 1: _t->setPitchCents(*reinterpret_cast<double*>(_v)); break;
        default: break;
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
        if (_id < 12)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 12;
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 12)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 12;
    }
    if (_c == QMetaObject::ReadProperty || _c == QMetaObject::WriteProperty
            || _c == QMetaObject::ResetProperty || _c == QMetaObject::BindableProperty
            || _c == QMetaObject::RegisterPropertyMetaType) {
        qt_static_metacall(this, _c, _id, _a);
        _id -= 2;
    }
    return _id;
}

// SIGNAL 0
void SCut::windowParamsChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 0, nullptr);
}
QT_WARNING_POP
