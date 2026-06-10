/****************************************************************************
** Meta object code from reading C++ file 'sproject.h'
**
** Created by: The Qt Meta Object Compiler version 69 (Qt 6.11.1)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../../../smaragd/main/include/sproject.h"
#include <QtCore/qmetatype.h>

#include <QtCore/qtmochelpers.h>

#include <memory>


#include <QtCore/qxptype_traits.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'sproject.h' doesn't include <QObject>."
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
struct qt_meta_tag_ZN8SProjectE_t {};
} // unnamed namespace

template <> constexpr inline auto SProject::qt_create_metaobjectdata<qt_meta_tag_ZN8SProjectE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "SProject",
        "fileNameChanged",
        "",
        "assetAdded",
        "name",
        "SObject&",
        "body",
        "assetRemoved",
        "arrangementChanged",
        "externFileAdded",
        "SExternFile",
        "externFileRemoved",
        "bpmTempoChanged",
        "sampleRateChanged",
        "propertyChanged",
        "key",
        "QVariant",
        "value",
        "setFileName",
        "addExternObject",
        "removeExternObject",
        "QString&",
        "setBPMTempo",
        "setSRate",
        "setCandidateRates",
        "std::vector<std::uint32_t>"
    };

    QtMocHelpers::UintData qt_methods {
        // Signal 'fileNameChanged'
        QtMocHelpers::SignalData<void(const QString &)>(1, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 2 },
        }}),
        // Signal 'assetAdded'
        QtMocHelpers::SignalData<void(const QString &, SObject &)>(3, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 4 }, { 0x80000000 | 5, 6 },
        }}),
        // Signal 'assetRemoved'
        QtMocHelpers::SignalData<void(const QString &)>(7, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 4 },
        }}),
        // Signal 'arrangementChanged'
        QtMocHelpers::SignalData<void()>(8, 2, QMC::AccessPublic, QMetaType::Void),
        // Signal 'externFileAdded'
        QtMocHelpers::SignalData<void(const SExternFile &)>(9, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 10, 2 },
        }}),
        // Signal 'externFileRemoved'
        QtMocHelpers::SignalData<void(const QString)>(11, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 2 },
        }}),
        // Signal 'bpmTempoChanged'
        QtMocHelpers::SignalData<void(double)>(12, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Double, 2 },
        }}),
        // Signal 'sampleRateChanged'
        QtMocHelpers::SignalData<void(int)>(13, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Int, 2 },
        }}),
        // Signal 'propertyChanged'
        QtMocHelpers::SignalData<void(const QString &, const QVariant &)>(14, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 15 }, { 0x80000000 | 16, 17 },
        }}),
        // Slot 'setFileName'
        QtMocHelpers::SlotData<void(const QString &)>(18, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 2 },
        }}),
        // Slot 'addExternObject'
        QtMocHelpers::SlotData<void(const SExternFile &)>(19, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 10, 2 },
        }}),
        // Slot 'removeExternObject'
        QtMocHelpers::SlotData<void(QString &)>(20, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 21, 2 },
        }}),
        // Slot 'setBPMTempo'
        QtMocHelpers::SlotData<void(double)>(22, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Double, 2 },
        }}),
        // Slot 'setSRate'
        QtMocHelpers::SlotData<void(int)>(23, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Int, 2 },
        }}),
        // Slot 'setCandidateRates'
        QtMocHelpers::SlotData<void(std::vector<std::uint32_t>)>(24, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 25, 2 },
        }}),
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<SProject, qt_meta_tag_ZN8SProjectE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject SProject::staticMetaObject = { {
    QMetaObject::SuperData::link<QObject::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN8SProjectE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN8SProjectE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN8SProjectE_t>.metaTypes,
    nullptr
} };

void SProject::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<SProject *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->fileNameChanged((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1]))); break;
        case 1: _t->assetAdded((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<SObject&>>(_a[2]))); break;
        case 2: _t->assetRemoved((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1]))); break;
        case 3: _t->arrangementChanged(); break;
        case 4: _t->externFileAdded((*reinterpret_cast<std::add_pointer_t<SExternFile>>(_a[1]))); break;
        case 5: _t->externFileRemoved((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1]))); break;
        case 6: _t->bpmTempoChanged((*reinterpret_cast<std::add_pointer_t<double>>(_a[1]))); break;
        case 7: _t->sampleRateChanged((*reinterpret_cast<std::add_pointer_t<int>>(_a[1]))); break;
        case 8: _t->propertyChanged((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<QVariant>>(_a[2]))); break;
        case 9: _t->setFileName((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1]))); break;
        case 10: _t->addExternObject((*reinterpret_cast<std::add_pointer_t<SExternFile>>(_a[1]))); break;
        case 11: _t->removeExternObject((*reinterpret_cast<std::add_pointer_t<QString&>>(_a[1]))); break;
        case 12: _t->setBPMTempo((*reinterpret_cast<std::add_pointer_t<double>>(_a[1]))); break;
        case 13: _t->setSRate((*reinterpret_cast<std::add_pointer_t<int>>(_a[1]))); break;
        case 14: _t->setCandidateRates((*reinterpret_cast<std::add_pointer_t<std::vector<std::uint32_t>>>(_a[1]))); break;
        default: ;
        }
    }
    if (_c == QMetaObject::IndexOfMethod) {
        if (QtMocHelpers::indexOfMethod<void (SProject::*)(const QString & )>(_a, &SProject::fileNameChanged, 0))
            return;
        if (QtMocHelpers::indexOfMethod<void (SProject::*)(const QString & , SObject & )>(_a, &SProject::assetAdded, 1))
            return;
        if (QtMocHelpers::indexOfMethod<void (SProject::*)(const QString & )>(_a, &SProject::assetRemoved, 2))
            return;
        if (QtMocHelpers::indexOfMethod<void (SProject::*)()>(_a, &SProject::arrangementChanged, 3))
            return;
        if (QtMocHelpers::indexOfMethod<void (SProject::*)(const SExternFile & )>(_a, &SProject::externFileAdded, 4))
            return;
        if (QtMocHelpers::indexOfMethod<void (SProject::*)(const QString )>(_a, &SProject::externFileRemoved, 5))
            return;
        if (QtMocHelpers::indexOfMethod<void (SProject::*)(double )>(_a, &SProject::bpmTempoChanged, 6))
            return;
        if (QtMocHelpers::indexOfMethod<void (SProject::*)(int )>(_a, &SProject::sampleRateChanged, 7))
            return;
        if (QtMocHelpers::indexOfMethod<void (SProject::*)(const QString & , const QVariant & )>(_a, &SProject::propertyChanged, 8))
            return;
    }
}

const QMetaObject *SProject::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *SProject::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN8SProjectE_t>.strings))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int SProject::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 15)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 15;
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 15)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 15;
    }
    return _id;
}

// SIGNAL 0
void SProject::fileNameChanged(const QString & _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 0, nullptr, _t1);
}

// SIGNAL 1
void SProject::assetAdded(const QString & _t1, SObject & _t2)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 1, nullptr, _t1, _t2);
}

// SIGNAL 2
void SProject::assetRemoved(const QString & _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 2, nullptr, _t1);
}

// SIGNAL 3
void SProject::arrangementChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 3, nullptr);
}

// SIGNAL 4
void SProject::externFileAdded(const SExternFile & _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 4, nullptr, _t1);
}

// SIGNAL 5
void SProject::externFileRemoved(const QString _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 5, nullptr, _t1);
}

// SIGNAL 6
void SProject::bpmTempoChanged(double _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 6, nullptr, _t1);
}

// SIGNAL 7
void SProject::sampleRateChanged(int _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 7, nullptr, _t1);
}

// SIGNAL 8
void SProject::propertyChanged(const QString & _t1, const QVariant & _t2)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 8, nullptr, _t1, _t2);
}
QT_WARNING_POP
