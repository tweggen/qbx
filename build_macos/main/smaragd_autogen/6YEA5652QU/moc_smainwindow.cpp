/****************************************************************************
** Meta object code from reading C++ file 'smainwindow.h'
**
** Created by: The Qt Meta Object Compiler version 69 (Qt 6.11.1)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../../../smaragd/main/include/smainwindow.h"
#include <QtCore/qmetatype.h>

#include <QtCore/qtmochelpers.h>

#include <memory>


#include <QtCore/qxptype_traits.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'smainwindow.h' doesn't include <QObject>."
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
struct qt_meta_tag_ZN11SMainWindowE_t {};
} // unnamed namespace

template <> constexpr inline auto SMainWindow::qt_create_metaobjectdata<qt_meta_tag_ZN11SMainWindowE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "SMainWindow",
        "nyi",
        "",
        "fileExit",
        "fileNew",
        "fileSave",
        "fileSaveAs",
        "fileOpen",
        "fileClose",
        "startPlaying",
        "stopPlaying",
        "audioDeviceSelected",
        "QAction*",
        "runTestSequence",
        "runVolumeBurst",
        "runSaveLoadTest",
        "runGroupTrackTest",
        "runReorderTrackTest",
        "runGroupPersist",
        "runUndoRemoveTest",
        "runSetClipStretch",
        "runSetClipPitch",
        "undo",
        "redo",
        "showOptionsDialog",
        "toggleSnapToGrid",
        "toggleGrid",
        "toggleMetronome",
        "toggleCycle",
        "groupTrack",
        "ungroupTrack",
        "onProjectPropertyChanged",
        "key",
        "QVariant",
        "value",
        "onStatusModeChanged",
        "mode"
    };

    QtMocHelpers::UintData qt_methods {
        // Slot 'nyi'
        QtMocHelpers::SlotData<void()>(1, 2, QMC::AccessProtected, QMetaType::Void),
        // Slot 'fileExit'
        QtMocHelpers::SlotData<void()>(3, 2, QMC::AccessProtected, QMetaType::Void),
        // Slot 'fileNew'
        QtMocHelpers::SlotData<void()>(4, 2, QMC::AccessProtected, QMetaType::Void),
        // Slot 'fileSave'
        QtMocHelpers::SlotData<void()>(5, 2, QMC::AccessProtected, QMetaType::Void),
        // Slot 'fileSaveAs'
        QtMocHelpers::SlotData<void()>(6, 2, QMC::AccessProtected, QMetaType::Void),
        // Slot 'fileOpen'
        QtMocHelpers::SlotData<void()>(7, 2, QMC::AccessProtected, QMetaType::Void),
        // Slot 'fileClose'
        QtMocHelpers::SlotData<void()>(8, 2, QMC::AccessProtected, QMetaType::Void),
        // Slot 'startPlaying'
        QtMocHelpers::SlotData<void()>(9, 2, QMC::AccessProtected, QMetaType::Void),
        // Slot 'stopPlaying'
        QtMocHelpers::SlotData<void()>(10, 2, QMC::AccessProtected, QMetaType::Void),
        // Slot 'audioDeviceSelected'
        QtMocHelpers::SlotData<void(QAction *)>(11, 2, QMC::AccessProtected, QMetaType::Void, {{
            { 0x80000000 | 12, 2 },
        }}),
        // Slot 'runTestSequence'
        QtMocHelpers::SlotData<void()>(13, 2, QMC::AccessProtected, QMetaType::Void),
        // Slot 'runVolumeBurst'
        QtMocHelpers::SlotData<void()>(14, 2, QMC::AccessProtected, QMetaType::Void),
        // Slot 'runSaveLoadTest'
        QtMocHelpers::SlotData<void()>(15, 2, QMC::AccessProtected, QMetaType::Void),
        // Slot 'runGroupTrackTest'
        QtMocHelpers::SlotData<void()>(16, 2, QMC::AccessProtected, QMetaType::Void),
        // Slot 'runReorderTrackTest'
        QtMocHelpers::SlotData<void()>(17, 2, QMC::AccessProtected, QMetaType::Void),
        // Slot 'runGroupPersist'
        QtMocHelpers::SlotData<void()>(18, 2, QMC::AccessProtected, QMetaType::Void),
        // Slot 'runUndoRemoveTest'
        QtMocHelpers::SlotData<void()>(19, 2, QMC::AccessProtected, QMetaType::Void),
        // Slot 'runSetClipStretch'
        QtMocHelpers::SlotData<void()>(20, 2, QMC::AccessProtected, QMetaType::Void),
        // Slot 'runSetClipPitch'
        QtMocHelpers::SlotData<void()>(21, 2, QMC::AccessProtected, QMetaType::Void),
        // Slot 'undo'
        QtMocHelpers::SlotData<void()>(22, 2, QMC::AccessProtected, QMetaType::Void),
        // Slot 'redo'
        QtMocHelpers::SlotData<void()>(23, 2, QMC::AccessProtected, QMetaType::Void),
        // Slot 'showOptionsDialog'
        QtMocHelpers::SlotData<void()>(24, 2, QMC::AccessProtected, QMetaType::Void),
        // Slot 'toggleSnapToGrid'
        QtMocHelpers::SlotData<void()>(25, 2, QMC::AccessProtected, QMetaType::Void),
        // Slot 'toggleGrid'
        QtMocHelpers::SlotData<void()>(26, 2, QMC::AccessProtected, QMetaType::Void),
        // Slot 'toggleMetronome'
        QtMocHelpers::SlotData<void()>(27, 2, QMC::AccessProtected, QMetaType::Void),
        // Slot 'toggleCycle'
        QtMocHelpers::SlotData<void()>(28, 2, QMC::AccessProtected, QMetaType::Void),
        // Slot 'groupTrack'
        QtMocHelpers::SlotData<void()>(29, 2, QMC::AccessProtected, QMetaType::Void),
        // Slot 'ungroupTrack'
        QtMocHelpers::SlotData<void()>(30, 2, QMC::AccessProtected, QMetaType::Void),
        // Slot 'onProjectPropertyChanged'
        QtMocHelpers::SlotData<void(const QString &, const QVariant &)>(31, 2, QMC::AccessProtected, QMetaType::Void, {{
            { QMetaType::QString, 32 }, { 0x80000000 | 33, 34 },
        }}),
        // Slot 'onStatusModeChanged'
        QtMocHelpers::SlotData<void(const QString &)>(35, 2, QMC::AccessProtected, QMetaType::Void, {{
            { QMetaType::QString, 36 },
        }}),
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<SMainWindow, qt_meta_tag_ZN11SMainWindowE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject SMainWindow::staticMetaObject = { {
    QMetaObject::SuperData::link<QMainWindow::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN11SMainWindowE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN11SMainWindowE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN11SMainWindowE_t>.metaTypes,
    nullptr
} };

void SMainWindow::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<SMainWindow *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->nyi(); break;
        case 1: _t->fileExit(); break;
        case 2: _t->fileNew(); break;
        case 3: _t->fileSave(); break;
        case 4: _t->fileSaveAs(); break;
        case 5: _t->fileOpen(); break;
        case 6: _t->fileClose(); break;
        case 7: _t->startPlaying(); break;
        case 8: _t->stopPlaying(); break;
        case 9: _t->audioDeviceSelected((*reinterpret_cast<std::add_pointer_t<QAction*>>(_a[1]))); break;
        case 10: _t->runTestSequence(); break;
        case 11: _t->runVolumeBurst(); break;
        case 12: _t->runSaveLoadTest(); break;
        case 13: _t->runGroupTrackTest(); break;
        case 14: _t->runReorderTrackTest(); break;
        case 15: _t->runGroupPersist(); break;
        case 16: _t->runUndoRemoveTest(); break;
        case 17: _t->runSetClipStretch(); break;
        case 18: _t->runSetClipPitch(); break;
        case 19: _t->undo(); break;
        case 20: _t->redo(); break;
        case 21: _t->showOptionsDialog(); break;
        case 22: _t->toggleSnapToGrid(); break;
        case 23: _t->toggleGrid(); break;
        case 24: _t->toggleMetronome(); break;
        case 25: _t->toggleCycle(); break;
        case 26: _t->groupTrack(); break;
        case 27: _t->ungroupTrack(); break;
        case 28: _t->onProjectPropertyChanged((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<QVariant>>(_a[2]))); break;
        case 29: _t->onStatusModeChanged((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1]))); break;
        default: ;
        }
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        switch (_id) {
        default: *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType(); break;
        case 9:
            switch (*reinterpret_cast<int*>(_a[1])) {
            default: *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType(); break;
            case 0:
                *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType::fromType< QAction* >(); break;
            }
            break;
        }
    }
}

const QMetaObject *SMainWindow::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *SMainWindow::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN11SMainWindowE_t>.strings))
        return static_cast<void*>(this);
    return QMainWindow::qt_metacast(_clname);
}

int SMainWindow::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QMainWindow::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 30)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 30;
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 30)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 30;
    }
    return _id;
}
QT_WARNING_POP
