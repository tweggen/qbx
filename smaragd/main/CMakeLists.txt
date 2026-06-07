set(SMARAGD_HEADERS
    include/sapplication.h
    include/scut.h
    include/scutrndrinline.h
    include/sexternfile.h
    include/sexternfilelist.h
    include/sgrainfile.h
    include/sgrainfilerndrinline.h
    include/slink.h
    include/smainwindow.h
    include/sobject.h
    include/sobjectrenderer.h
    include/sobjecttreeview.h
    include/splainwave.h
    include/splainwaverndrinline.h
    include/sproject.h
    include/sprojectloader.h
    include/ssettings.h
    include/ssmvmixercontrol.h
    include/sstdmixer.h
    include/sstdmixerview.h
    include/strack.h
    include/strackrenderer.h
    include/strackrndrinline.h
    include/saction.h
    include/sactionregistry.h
    include/sactionqueue.h
    include/sactionhistory.h
    include/sactionundocommand.h
    include/actions/saddtrackaction.h
    include/actions/sremovetrackaction.h
    include/actions/srestoretrackaction.h
    include/actions/sreparenttrackaction.h
    include/actions/smovetrackaction.h
    include/actions/smoveclipaction.h
    include/actions/ssplitclipaction.h
    include/actions/sunsplitclipaction.h
    include/actions/strackpath.h
    include/actions/saddsampleaction.h
    include/actions/sremovesampleaction.h
    include/actions/ssettrackvolumeaction.h
    include/actions/ssaveprojectaction.h
    include/actions/sloadprojectaction.h
    include/actions/stogglesettingaction.h
    include/actions/ssnaptogridaction.h
    include/actions/sgridaction.h
    include/actions/smetronomeaction.h
    include/actions/scycleaction.h
    include/actions/ssetpropertyaction.h
    include/actions/stoggleplaybackaction.h
    include/sprojectprops.h
)

set(SMARAGD_SOURCES
    src/main.cpp
    src/sapplication.cpp
    src/scut.cpp
    src/scutrndrinline.cpp
    src/sexternfile.cpp
    src/sexternfilelist.cpp
    src/sgrainfile.cpp
    src/sgrainfilerndrinline.cpp
    src/slink.cpp
    src/smainwindow.cpp
    src/sobject.cpp
    src/sobjectrenderer.cpp
    src/sobjecttreeview.cpp
    src/splainwave.cpp
    src/splainwaverndrinline.cpp
    src/sproject.cpp
    src/sprojectloader.cpp
    src/ssettings.cpp
    src/ssmvmixercontrol.cpp
    src/sstdmixer.cpp
    src/sstdmixerview.cpp
    src/strack.cpp
    src/strackrndrinline.cpp
    src/sactionregistry.cpp
    src/sactionqueue.cpp
    src/sactionhistory.cpp
    src/sactionundocommand.cpp
    src/actions/saddtrackaction.cpp
    src/actions/sremovetrackaction.cpp
    src/actions/srestoretrackaction.cpp
    src/actions/sreparenttrackaction.cpp
    src/actions/smovetrackaction.cpp
    src/actions/smoveclipaction.cpp
    src/actions/ssplitclipaction.cpp
    src/actions/sunsplitclipaction.cpp
    src/actions/saddsampleaction.cpp
    src/actions/sremovesampleaction.cpp
    src/actions/ssettrackvolumeaction.cpp
    src/actions/ssaveprojectaction.cpp
    src/actions/sloadprojectaction.cpp
    src/actions/ssnaptogridaction.cpp
    src/actions/sgridaction.cpp
    src/actions/smetronomeaction.cpp
    src/actions/scycleaction.cpp
    src/actions/ssetpropertyaction.cpp
    src/actions/stoggleplaybackaction.cpp
)

# SMARAGD_WIN_CONSOLE: when ON (the default), the Windows build uses the
# CONSOLE subsystem so stderr/stdout — including the twsyslog.h shim's
# vfprintf calls and Qt's qDebug() — appear in the launching console.
# Flip to OFF for a release build that should not pop a console window.
option(SMARAGD_WIN_CONSOLE
    "Build the Windows binary against the CONSOLE subsystem (shows stderr)" ON)

if(SMARAGD_WINDOWS AND NOT SMARAGD_WIN_CONSOLE)
    add_executable(smaragd WIN32 ${SMARAGD_SOURCES} ${SMARAGD_HEADERS})
elseif(SMARAGD_MACOS)
    add_executable(smaragd MACOSX_BUNDLE ${SMARAGD_SOURCES} ${SMARAGD_HEADERS})
else()
    add_executable(smaragd ${SMARAGD_SOURCES} ${SMARAGD_HEADERS})
endif()

target_include_directories(smaragd
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}
)

target_link_libraries(smaragd
    PRIVATE
        tw303a
        Qt${QT_VERSION_MAJOR}::Core
        Qt${QT_VERSION_MAJOR}::Widgets
        Qt${QT_VERSION_MAJOR}::Xml
)

# Auto-deploy Qt runtime DLLs / plugins next to smaragd.exe on Windows
# (and the .app bundle on macOS) so the binary launches without a manual
# windeployqt step. Opt out with -DAUTO_DEPLOY_QT=OFF.
option(AUTO_DEPLOY_QT
    "Run windeployqt / macdeployqt after building smaragd" ON)

if(AUTO_DEPLOY_QT AND (SMARAGD_WINDOWS OR SMARAGD_MACOS))
    # windeployqt / macdeployqt live next to qmake in the Qt bin dir.
    get_target_property(_qt_qmake Qt${QT_VERSION_MAJOR}::qmake IMPORTED_LOCATION)
    get_filename_component(_qt_bin_dir "${_qt_qmake}" DIRECTORY)

    if(SMARAGD_WINDOWS)
        find_program(DEPLOYQT_EXECUTABLE windeployqt HINTS "${_qt_bin_dir}")
        set(DEPLOYQT_ARGS --no-translations --no-system-d3d-compiler --no-opengl-sw)
    else()
        find_program(DEPLOYQT_EXECUTABLE macdeployqt HINTS "${_qt_bin_dir}")
        set(DEPLOYQT_ARGS "")
    endif()

    if(DEPLOYQT_EXECUTABLE)
        add_custom_command(TARGET smaragd POST_BUILD
            COMMAND "${DEPLOYQT_EXECUTABLE}" ${DEPLOYQT_ARGS} "$<TARGET_FILE:smaragd>"
            COMMENT "Deploying Qt runtime alongside $<TARGET_FILE_NAME:smaragd>"
            VERBATIM
        )
    else()
        message(WARNING
            "AUTO_DEPLOY_QT is ON but the deploy tool was not found in "
            "${_qt_bin_dir}. The binary will need its Qt DLLs supplied "
            "manually (PATH or copy).")
    endif()
endif()
