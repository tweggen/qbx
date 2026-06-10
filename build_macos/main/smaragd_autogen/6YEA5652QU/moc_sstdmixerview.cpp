/****************************************************************************
** Meta object code from reading C++ file 'sstdmixerview.h'
**
** Created by: The Qt Meta Object Compiler version 69 (Qt 6.11.1)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../../../smaragd/main/include/sstdmixerview.h"
#include <QtCore/qmetatype.h>

#include <QtCore/qtmochelpers.h>

#include <memory>


#include <QtCore/qxptype_traits.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'sstdmixerview.h' doesn't include <QObject>."
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
struct qt_meta_tag_ZN13SMVActualViewE_t {};
} // unnamed namespace

template <> constexpr inline auto SMVActualView::qt_create_metaobjectdata<qt_meta_tag_ZN13SMVActualViewE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "SMVActualView",
        "trackHeightChanged",
        "",
        "x",
        "secondWidthChanged",
        "leftOffsetChanged",
        "offset_t",
        "topOffsetChanged",
        "setTrackHeight",
        "setSecondWidth",
        "setUpperLeft",
        "upperLeftX",
        "idx_t",
        "offsetLeftY",
        "setLeftOffset",
        "setTopOffset",
        "ctGlobalShow",
        "ctRangeSetBPM",
        "ctRangeClear",
        "ctCreateAssetFromTrack",
        "globalLocatorMoved"
    };

    QtMocHelpers::UintData qt_methods {
        // Signal 'trackHeightChanged'
        QtMocHelpers::SignalData<void(int)>(1, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Int, 3 },
        }}),
        // Signal 'secondWidthChanged'
        QtMocHelpers::SignalData<void(int)>(4, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Int, 3 },
        }}),
        // Signal 'leftOffsetChanged'
        QtMocHelpers::SignalData<void(offset_t)>(5, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 6, 2 },
        }}),
        // Signal 'topOffsetChanged'
        QtMocHelpers::SignalData<void(offset_t)>(7, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 6, 2 },
        }}),
        // Slot 'setTrackHeight'
        QtMocHelpers::SlotData<void(int)>(8, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Int, 3 },
        }}),
        // Slot 'setSecondWidth'
        QtMocHelpers::SlotData<void(double)>(9, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Double, 3 },
        }}),
        // Slot 'setUpperLeft'
        QtMocHelpers::SlotData<void(offset_t, idx_t)>(10, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 6, 11 }, { 0x80000000 | 12, 13 },
        }}),
        // Slot 'setLeftOffset'
        QtMocHelpers::SlotData<void(offset_t)>(14, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 6, 2 },
        }}),
        // Slot 'setTopOffset'
        QtMocHelpers::SlotData<void(idx_t)>(15, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 12, 2 },
        }}),
        // Slot 'ctGlobalShow'
        QtMocHelpers::SlotData<void()>(16, 2, QMC::AccessProtected, QMetaType::Void),
        // Slot 'ctRangeSetBPM'
        QtMocHelpers::SlotData<void()>(17, 2, QMC::AccessProtected, QMetaType::Void),
        // Slot 'ctRangeClear'
        QtMocHelpers::SlotData<void()>(18, 2, QMC::AccessProtected, QMetaType::Void),
        // Slot 'ctCreateAssetFromTrack'
        QtMocHelpers::SlotData<void()>(19, 2, QMC::AccessProtected, QMetaType::Void),
        // Slot 'globalLocatorMoved'
        QtMocHelpers::SlotData<void(offset_t, offset_t)>(20, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { 0x80000000 | 6, 2 }, { 0x80000000 | 6, 2 },
        }}),
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<SMVActualView, qt_meta_tag_ZN13SMVActualViewE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject SMVActualView::staticMetaObject = { {
    QMetaObject::SuperData::link<QWidget::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN13SMVActualViewE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN13SMVActualViewE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN13SMVActualViewE_t>.metaTypes,
    nullptr
} };

void SMVActualView::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<SMVActualView *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->trackHeightChanged((*reinterpret_cast<std::add_pointer_t<int>>(_a[1]))); break;
        case 1: _t->secondWidthChanged((*reinterpret_cast<std::add_pointer_t<int>>(_a[1]))); break;
        case 2: _t->leftOffsetChanged((*reinterpret_cast<std::add_pointer_t<offset_t>>(_a[1]))); break;
        case 3: _t->topOffsetChanged((*reinterpret_cast<std::add_pointer_t<offset_t>>(_a[1]))); break;
        case 4: _t->setTrackHeight((*reinterpret_cast<std::add_pointer_t<int>>(_a[1]))); break;
        case 5: _t->setSecondWidth((*reinterpret_cast<std::add_pointer_t<double>>(_a[1]))); break;
        case 6: _t->setUpperLeft((*reinterpret_cast<std::add_pointer_t<offset_t>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<idx_t>>(_a[2]))); break;
        case 7: _t->setLeftOffset((*reinterpret_cast<std::add_pointer_t<offset_t>>(_a[1]))); break;
        case 8: _t->setTopOffset((*reinterpret_cast<std::add_pointer_t<idx_t>>(_a[1]))); break;
        case 9: _t->ctGlobalShow(); break;
        case 10: _t->ctRangeSetBPM(); break;
        case 11: _t->ctRangeClear(); break;
        case 12: _t->ctCreateAssetFromTrack(); break;
        case 13: _t->globalLocatorMoved((*reinterpret_cast<std::add_pointer_t<offset_t>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<offset_t>>(_a[2]))); break;
        default: ;
        }
    }
    if (_c == QMetaObject::IndexOfMethod) {
        if (QtMocHelpers::indexOfMethod<void (SMVActualView::*)(int )>(_a, &SMVActualView::trackHeightChanged, 0))
            return;
        if (QtMocHelpers::indexOfMethod<void (SMVActualView::*)(int )>(_a, &SMVActualView::secondWidthChanged, 1))
            return;
        if (QtMocHelpers::indexOfMethod<void (SMVActualView::*)(offset_t )>(_a, &SMVActualView::leftOffsetChanged, 2))
            return;
        if (QtMocHelpers::indexOfMethod<void (SMVActualView::*)(offset_t )>(_a, &SMVActualView::topOffsetChanged, 3))
            return;
    }
}

const QMetaObject *SMVActualView::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *SMVActualView::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN13SMVActualViewE_t>.strings))
        return static_cast<void*>(this);
    return QWidget::qt_metacast(_clname);
}

int SMVActualView::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QWidget::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 14)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 14;
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 14)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 14;
    }
    return _id;
}

// SIGNAL 0
void SMVActualView::trackHeightChanged(int _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 0, nullptr, _t1);
}

// SIGNAL 1
void SMVActualView::secondWidthChanged(int _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 1, nullptr, _t1);
}

// SIGNAL 2
void SMVActualView::leftOffsetChanged(offset_t _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 2, nullptr, _t1);
}

// SIGNAL 3
void SMVActualView::topOffsetChanged(offset_t _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 3, nullptr, _t1);
}
namespace {
struct qt_meta_tag_ZN9SSnapSpecE_t {};
} // unnamed namespace

template <> constexpr inline auto SSnapSpec::qt_create_metaobjectdata<qt_meta_tag_ZN9SSnapSpecE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "SSnapSpec",
        "snapMethodChanged",
        "",
        "beatSubDivChanged",
        "idx_t",
        "setBeatSubDiv",
        "setSnapMethod"
    };

    QtMocHelpers::UintData qt_methods {
        // Signal 'snapMethodChanged'
        QtMocHelpers::SignalData<void(int)>(1, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Int, 2 },
        }}),
        // Signal 'beatSubDivChanged'
        QtMocHelpers::SignalData<void(idx_t)>(3, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 4, 2 },
        }}),
        // Slot 'setBeatSubDiv'
        QtMocHelpers::SlotData<void(idx_t)>(5, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 4, 2 },
        }}),
        // Slot 'setSnapMethod'
        QtMocHelpers::SlotData<void(int)>(6, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Int, 2 },
        }}),
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<SSnapSpec, qt_meta_tag_ZN9SSnapSpecE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject SSnapSpec::staticMetaObject = { {
    QMetaObject::SuperData::link<QObject::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN9SSnapSpecE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN9SSnapSpecE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN9SSnapSpecE_t>.metaTypes,
    nullptr
} };

void SSnapSpec::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<SSnapSpec *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->snapMethodChanged((*reinterpret_cast<std::add_pointer_t<int>>(_a[1]))); break;
        case 1: _t->beatSubDivChanged((*reinterpret_cast<std::add_pointer_t<idx_t>>(_a[1]))); break;
        case 2: _t->setBeatSubDiv((*reinterpret_cast<std::add_pointer_t<idx_t>>(_a[1]))); break;
        case 3: _t->setSnapMethod((*reinterpret_cast<std::add_pointer_t<int>>(_a[1]))); break;
        default: ;
        }
    }
    if (_c == QMetaObject::IndexOfMethod) {
        if (QtMocHelpers::indexOfMethod<void (SSnapSpec::*)(int )>(_a, &SSnapSpec::snapMethodChanged, 0))
            return;
        if (QtMocHelpers::indexOfMethod<void (SSnapSpec::*)(idx_t )>(_a, &SSnapSpec::beatSubDivChanged, 1))
            return;
    }
}

const QMetaObject *SSnapSpec::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *SSnapSpec::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN9SSnapSpecE_t>.strings))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int SSnapSpec::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
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
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 4;
    }
    return _id;
}

// SIGNAL 0
void SSnapSpec::snapMethodChanged(int _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 0, nullptr, _t1);
}

// SIGNAL 1
void SSnapSpec::beatSubDivChanged(idx_t _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 1, nullptr, _t1);
}
namespace {
struct qt_meta_tag_ZN13SStdMixerViewE_t {};
} // unnamed namespace

template <> constexpr inline auto SStdMixerView::qt_create_metaobjectdata<qt_meta_tag_ZN13SStdMixerViewE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "SStdMixerView",
        "timeGridSpecChanged",
        "",
        "STimeGridSpec",
        "ctAddTrack",
        "ctRemoveTrack",
        "ctIndentTrack",
        "ctOutdentTrack",
        "ctGroupTrack",
        "ctUngroupTrack",
        "ctInsertSample",
        "ctRemoveSample",
        "ctDeleteSample",
        "ctSplitSample",
        "ctAddLink",
        "setTimeGridSpec",
        "setBPMTempo",
        "viewResized",
        "contentDurationChanged",
        "length_t",
        "newDuration",
        "nTracksChanged",
        "timeSliderMoved",
        "newValue",
        "trackSliderMoved",
        "zoomOutHor",
        "zoomInHor",
        "zoomOutVert",
        "zoomInVert",
        "avLeftOffsetChanged",
        "offset_t",
        "addMixerControl",
        "STrack&",
        "removeMixerControl",
        "tracksReordered"
    };

    QtMocHelpers::UintData qt_methods {
        // Signal 'timeGridSpecChanged'
        QtMocHelpers::SignalData<void(const STimeGridSpec &)>(1, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 3, 2 },
        }}),
        // Slot 'ctAddTrack'
        QtMocHelpers::SlotData<void()>(4, 2, QMC::AccessPublic, QMetaType::Void),
        // Slot 'ctRemoveTrack'
        QtMocHelpers::SlotData<void()>(5, 2, QMC::AccessPublic, QMetaType::Void),
        // Slot 'ctIndentTrack'
        QtMocHelpers::SlotData<void()>(6, 2, QMC::AccessPublic, QMetaType::Void),
        // Slot 'ctOutdentTrack'
        QtMocHelpers::SlotData<void()>(7, 2, QMC::AccessPublic, QMetaType::Void),
        // Slot 'ctGroupTrack'
        QtMocHelpers::SlotData<void()>(8, 2, QMC::AccessPublic, QMetaType::Void),
        // Slot 'ctUngroupTrack'
        QtMocHelpers::SlotData<void()>(9, 2, QMC::AccessPublic, QMetaType::Void),
        // Slot 'ctInsertSample'
        QtMocHelpers::SlotData<void()>(10, 2, QMC::AccessPublic, QMetaType::Void),
        // Slot 'ctRemoveSample'
        QtMocHelpers::SlotData<void()>(11, 2, QMC::AccessPublic, QMetaType::Void),
        // Slot 'ctDeleteSample'
        QtMocHelpers::SlotData<void()>(12, 2, QMC::AccessPublic, QMetaType::Void),
        // Slot 'ctSplitSample'
        QtMocHelpers::SlotData<void()>(13, 2, QMC::AccessPublic, QMetaType::Void),
        // Slot 'ctAddLink'
        QtMocHelpers::SlotData<void()>(14, 2, QMC::AccessPublic, QMetaType::Void),
        // Slot 'setTimeGridSpec'
        QtMocHelpers::SlotData<void(const STimeGridSpec &)>(15, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 3, 2 },
        }}),
        // Slot 'setBPMTempo'
        QtMocHelpers::SlotData<void(double)>(16, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Double, 2 },
        }}),
        // Slot 'viewResized'
        QtMocHelpers::SlotData<void()>(17, 2, QMC::AccessProtected, QMetaType::Void),
        // Slot 'contentDurationChanged'
        QtMocHelpers::SlotData<void(length_t)>(18, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { 0x80000000 | 19, 20 },
        }}),
        // Slot 'nTracksChanged'
        QtMocHelpers::SlotData<void()>(21, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'timeSliderMoved'
        QtMocHelpers::SlotData<void(int)>(22, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { QMetaType::Int, 23 },
        }}),
        // Slot 'trackSliderMoved'
        QtMocHelpers::SlotData<void(int)>(24, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { QMetaType::Int, 23 },
        }}),
        // Slot 'zoomOutHor'
        QtMocHelpers::SlotData<void()>(25, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'zoomInHor'
        QtMocHelpers::SlotData<void()>(26, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'zoomOutVert'
        QtMocHelpers::SlotData<void()>(27, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'zoomInVert'
        QtMocHelpers::SlotData<void()>(28, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'avLeftOffsetChanged'
        QtMocHelpers::SlotData<void(offset_t)>(29, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { 0x80000000 | 30, 2 },
        }}),
        // Slot 'addMixerControl'
        QtMocHelpers::SlotData<void(int, STrack &)>(31, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { QMetaType::Int, 2 }, { 0x80000000 | 32, 2 },
        }}),
        // Slot 'removeMixerControl'
        QtMocHelpers::SlotData<void(int, STrack &)>(33, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { QMetaType::Int, 2 }, { 0x80000000 | 32, 2 },
        }}),
        // Slot 'tracksReordered'
        QtMocHelpers::SlotData<void()>(34, 2, QMC::AccessPrivate, QMetaType::Void),
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<SStdMixerView, qt_meta_tag_ZN13SStdMixerViewE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject SStdMixerView::staticMetaObject = { {
    QMetaObject::SuperData::link<QWidget::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN13SStdMixerViewE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN13SStdMixerViewE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN13SStdMixerViewE_t>.metaTypes,
    nullptr
} };

void SStdMixerView::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<SStdMixerView *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->timeGridSpecChanged((*reinterpret_cast<std::add_pointer_t<STimeGridSpec>>(_a[1]))); break;
        case 1: _t->ctAddTrack(); break;
        case 2: _t->ctRemoveTrack(); break;
        case 3: _t->ctIndentTrack(); break;
        case 4: _t->ctOutdentTrack(); break;
        case 5: _t->ctGroupTrack(); break;
        case 6: _t->ctUngroupTrack(); break;
        case 7: _t->ctInsertSample(); break;
        case 8: _t->ctRemoveSample(); break;
        case 9: _t->ctDeleteSample(); break;
        case 10: _t->ctSplitSample(); break;
        case 11: _t->ctAddLink(); break;
        case 12: _t->setTimeGridSpec((*reinterpret_cast<std::add_pointer_t<STimeGridSpec>>(_a[1]))); break;
        case 13: _t->setBPMTempo((*reinterpret_cast<std::add_pointer_t<double>>(_a[1]))); break;
        case 14: _t->viewResized(); break;
        case 15: _t->contentDurationChanged((*reinterpret_cast<std::add_pointer_t<length_t>>(_a[1]))); break;
        case 16: _t->nTracksChanged(); break;
        case 17: _t->timeSliderMoved((*reinterpret_cast<std::add_pointer_t<int>>(_a[1]))); break;
        case 18: _t->trackSliderMoved((*reinterpret_cast<std::add_pointer_t<int>>(_a[1]))); break;
        case 19: _t->zoomOutHor(); break;
        case 20: _t->zoomInHor(); break;
        case 21: _t->zoomOutVert(); break;
        case 22: _t->zoomInVert(); break;
        case 23: _t->avLeftOffsetChanged((*reinterpret_cast<std::add_pointer_t<offset_t>>(_a[1]))); break;
        case 24: _t->addMixerControl((*reinterpret_cast<std::add_pointer_t<int>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<STrack&>>(_a[2]))); break;
        case 25: _t->removeMixerControl((*reinterpret_cast<std::add_pointer_t<int>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<STrack&>>(_a[2]))); break;
        case 26: _t->tracksReordered(); break;
        default: ;
        }
    }
    if (_c == QMetaObject::IndexOfMethod) {
        if (QtMocHelpers::indexOfMethod<void (SStdMixerView::*)(const STimeGridSpec & )>(_a, &SStdMixerView::timeGridSpecChanged, 0))
            return;
    }
}

const QMetaObject *SStdMixerView::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *SStdMixerView::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN13SStdMixerViewE_t>.strings))
        return static_cast<void*>(this);
    return QWidget::qt_metacast(_clname);
}

int SStdMixerView::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QWidget::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 27)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 27;
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 27)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 27;
    }
    return _id;
}

// SIGNAL 0
void SStdMixerView::timeGridSpecChanged(const STimeGridSpec & _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 0, nullptr, _t1);
}
QT_WARNING_POP
