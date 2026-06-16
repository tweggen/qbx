set(SMARAGD_HEADERS
    include/sapplication.h
    include/scut.h
    include/scutrndrinline.h
    include/sexternfile.h
    include/sexternfilelist.h
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
    include/soptions.h
    include/soptionsdialog.h
    include/srenderdialog.h
    include/srenderprogress.h
    include/srecordingprogress.h
    include/ssmvmixercontrol.h
    include/sstdmixer.h
    include/sstdmixerview.h
    include/strack.h
    include/strackrenderer.h
    include/strackrndrinline.h
    include/spluginchain.h
    include/spluginslot.h
    include/spluginbrowserdialog.h
    include/spluginparamereditor.h
    include/splugineffectstrip.h
    include/saction.h
    include/sactionregistry.h
    include/sactionqueue.h
    include/sactionhistory.h
    include/sactionundocommand.h
    include/sactionscript.h
    include/sactionrunner.h
    include/actions/saddtrackaction.h
    include/actions/sremovetrackaction.h
    include/actions/srestoretrackaction.h
    include/actions/sreparenttrackaction.h
    include/actions/smovetrackaction.h
    include/actions/smoveclipaction.h
    include/actions/ssplitclipaction.h
    include/actions/sunsplitclipaction.h
    include/actions/sduplicateclipaction.h
    include/actions/sremoveclipaction.h
    include/actions/sresizeclipaction.h
    include/actions/screateassetaction.h
    include/actions/sremoveassetaction.h
    include/actions/splaceassetaction.h
    include/actions/sremoveassetplacementaction.h
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
    include/sselectionmanager.h
    include/actions/ssetselectionaction.h
    include/actions/saddtoselectionaction.h
    include/actions/sremovefromselectionaction.h
    include/actions/sclearselectionaction.h
    include/actions/stoggleselectionaction.h
    include/actions/sinsertpluginaction.h
    include/actions/sremovepluginaction.h
    include/sprojectprops.h
)

set(SMARAGD_SOURCES
    src/main.cpp
    src/sapplication.cpp
    src/scut.cpp
    src/scutrndrinline.cpp
    src/sexternfile.cpp
    src/sexternfilelist.cpp
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
    src/soptions.cpp
    src/soptionsdialog.cpp
    src/srenderdialog.cpp
    src/srenderprogress.cpp
    src/srecordingprogress.cpp
    src/swaveformdraw.cpp
    src/ssmvmixercontrol.cpp
    src/sstdmixer.cpp
    src/sstdmixerview.cpp
    src/strack.cpp
    src/strackrndrinline.cpp
    src/spluginchain.cpp
    src/spluginslot.cpp
    src/spluginbrowserdialog.cpp
    src/spluginparamereditor.cpp
    src/splugineffectstrip.cpp
    src/sactionregistry.cpp
    src/sactionqueue.cpp
    src/sactionhistory.cpp
    src/sactionundocommand.cpp
    src/sactionscript.cpp
    src/sactionrunner.cpp
    src/actions/saddtrackaction.cpp
    src/actions/sremovetrackaction.cpp
    src/actions/srestoretrackaction.cpp
    src/actions/sreparenttrackaction.cpp
    src/actions/smovetrackaction.cpp
    src/actions/smoveclipaction.cpp
    src/actions/ssplitclipaction.cpp
    src/actions/sunsplitclipaction.cpp
    src/actions/sduplicateclipaction.cpp
    src/actions/sremoveclipaction.cpp
    src/actions/sresizeclipaction.cpp
    src/actions/screateassetaction.cpp
    src/actions/sremoveassetaction.cpp
    src/actions/splaceassetaction.cpp
    src/actions/sremoveassetplacementaction.cpp
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
    src/sselectionmanager.cpp
    src/actions/ssetselectionaction.cpp
    src/actions/saddtoselectionaction.cpp
    src/actions/sremovefromselectionaction.cpp
    src/actions/sclearselectionaction.cpp
    src/actions/stoggleselectionaction.cpp
    src/actions/sinsertpluginaction.cpp
    src/actions/sremovepluginaction.cpp
)

# Optionally embed a bundled UI font. Drop a TTF at main/resources/FreeSans.ttf
# and re-run CMake: it is compiled into the binary (AUTORCC) and used as the
# default, antialiased UI font (see main.cpp). If absent, the platform default
# font is used and the build is unaffected. (GNU FreeFont / FreeSans is GPL with
# a font exception, so bundling it in the application is fine.)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/resources/FreeSans.ttf)
    list(APPEND SMARAGD_SOURCES resources/fonts.qrc)
    message(STATUS "Smaragd: embedding bundled font resources/FreeSans.ttf")
else()
    message(STATUS "Smaragd: resources/FreeSans.ttf not found; using the platform default font")
endif()

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

    # Code-sign app with microphone entitlements after building
    add_custom_command(TARGET smaragd POST_BUILD
        COMMAND codesign --force --deep --sign -
                --entitlements "${CMAKE_CURRENT_SOURCE_DIR}/smaragd.entitlements"
                "$<TARGET_BUNDLE_DIR:smaragd>"
        COMMENT "Code-signing smaragd with microphone entitlements"
    )
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

# Action round-trip test (Phase 0 of action scripts): verify all actions
# can serialize and deserialize without loss.
# Exclude src/main.cpp since the test has its own main()
list(REMOVE_ITEM SMARAGD_SOURCES src/main.cpp)
add_executable(action_roundtrip_test
    src/action_roundtrip_test.cpp
    ${SMARAGD_SOURCES}
    ${SMARAGD_HEADERS}
)

target_include_directories(action_roundtrip_test
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}
)

target_link_libraries(action_roundtrip_test
    PRIVATE
        tw303a
        Qt${QT_VERSION_MAJOR}::Core
        Qt${QT_VERSION_MAJOR}::Widgets
        Qt${QT_VERSION_MAJOR}::Xml
)

# Exact arithmetic integration tests
add_executable(exact_arithmetic_test
    ../tw303a/src/test_exact_arithmetic.cpp
)

target_include_directories(exact_arithmetic_test
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../tw303a/include
        ${CMAKE_SOURCE_DIR}
)

target_link_libraries(exact_arithmetic_test
    PRIVATE
        tw303a
)

# Serialization roundtrip tests
add_executable(serialization_roundtrip_test
    ../tw303a/src/test_serialization_roundtrip.cpp
)

target_include_directories(serialization_roundtrip_test
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../tw303a/include
        ${CMAKE_SOURCE_DIR}
)

target_link_libraries(serialization_roundtrip_test
    PRIVATE
        tw303a
)
