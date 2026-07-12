# Smaragd application (proposal 14, Phase 2 app side).
#
# main/ is organized into module directories (app/<module>/... include paths):
#   model, objects/{cut,wave,track,mixer}, actions, persistence, selection,
#   timeline, pluginui, servicesui, shell, testkit
# The app is still ONE strongly-connected component (see proposal 14 —
# SApplication singleton, objects creating their own views, the loader knowing
# all types), so it builds as a single OBJECT library until the Phase 6
# interface work breaks the knot. An OBJECT library is deliberate: the actions
# self-register via static initializers, which a STATIC library would silently
# drop at link time. Module boundaries are checked review-time by
# tools/check_layering.py.

set(SMARAGD_APP_FILES
    # --- app/model
    model/include/app/model/sappcontext.h
    model/include/app/model/sdetaileditors.h
    model/include/app/model/sobjectpath.h
    model/src/sappcontext.cpp
    model/src/sdetaileditors.cpp
    model/include/app/model/sexternfile.h
    model/include/app/model/sexternfilelist.h
    model/include/app/model/slink.h
    model/include/app/model/sobject.h
    model/include/app/model/sobjectrenderer.h
    model/include/app/model/sproject.h
    model/include/app/model/sprojectprops.h
    model/include/app/model/ssortedobjlist.h
    model/src/sexternfile.cpp
    model/src/sexternfilelist.cpp
    model/src/slink.cpp
    model/src/sobject.cpp
    model/src/sobjectrenderer.cpp
    model/src/sproject.cpp
    # --- app/objects/cut
    objects/cut/include/app/objects/cut/scut.h
    objects/cut/include/app/objects/cut/scutrndrinline.h
    objects/cut/include/app/objects/cut/sduplicateclipaction.h
    objects/cut/include/app/objects/cut/sresizeclipaction.h
    objects/cut/include/app/objects/cut/ssplitclipaction.h
    objects/cut/include/app/objects/cut/sunsplitclipaction.h
    objects/cut/src/scut.cpp
    objects/cut/src/scutrndrinline.cpp
    objects/cut/src/sduplicateclipaction.cpp
    objects/cut/src/sresizeclipaction.cpp
    objects/cut/src/ssplitclipaction.cpp
    objects/cut/src/sunsplitclipaction.cpp
    # --- app/objects/wave
    objects/wave/include/app/objects/wave/saddsampleaction.h
    objects/wave/include/app/objects/wave/splainwave.h
    objects/wave/include/app/objects/wave/splainwaverndrinline.h
    objects/wave/include/app/objects/wave/sremovesampleaction.h
    objects/wave/include/app/objects/wave/swaveformdraw.h
    objects/wave/src/saddsampleaction.cpp
    objects/wave/src/splainwave.cpp
    objects/wave/src/splainwaverndrinline.cpp
    objects/wave/src/sremovesampleaction.cpp
    objects/wave/src/swaveformdraw.cpp
    # --- app/objects/track
    objects/track/include/app/objects/track/strackcolormodifier.h
    objects/track/src/strackcolormodifier.cpp
    objects/track/include/app/objects/track/saddtrackaction.h
    objects/track/include/app/objects/track/smoveclipaction.h
    objects/track/include/app/objects/track/smovetrackaction.h
    objects/track/include/app/objects/track/sremoveclipaction.h
    objects/track/include/app/objects/track/sremovetrackaction.h
    objects/track/include/app/objects/track/sreparenttrackaction.h
    objects/track/include/app/objects/track/srestoretrackaction.h
    objects/track/include/app/objects/track/ssettrackvolumeaction.h
    objects/track/include/app/objects/track/strack.h
    objects/track/include/app/objects/track/strackpath.h
    objects/track/include/app/objects/track/strackrenderer.h
    objects/track/include/app/objects/track/strackrndrinline.h
    objects/track/src/saddtrackaction.cpp
    objects/track/src/smoveclipaction.cpp
    objects/track/src/smovetrackaction.cpp
    objects/track/src/sremoveclipaction.cpp
    objects/track/src/sremovetrackaction.cpp
    objects/track/src/sreparenttrackaction.cpp
    objects/track/src/srestoretrackaction.cpp
    objects/track/src/ssettrackvolumeaction.cpp
    objects/track/src/strack.cpp
    objects/track/src/strackrndrinline.cpp
    # --- app/objects/mixer
    objects/mixer/include/app/objects/mixer/spluginslot.h
    objects/mixer/src/spluginslot.cpp
    objects/mixer/include/app/objects/mixer/screateassetaction.h
    objects/mixer/include/app/objects/mixer/sinsertpluginaction.h
    objects/mixer/include/app/objects/mixer/splaceassetaction.h
    objects/mixer/include/app/objects/mixer/spluginchain.h
    objects/mixer/include/app/objects/mixer/sremoveassetaction.h
    objects/mixer/include/app/objects/mixer/sremoveassetplacementaction.h
    objects/mixer/include/app/objects/mixer/sremovepluginaction.h
    objects/mixer/include/app/objects/mixer/sstdmixer.h
    objects/mixer/src/screateassetaction.cpp
    objects/mixer/src/sinsertpluginaction.cpp
    objects/mixer/src/splaceassetaction.cpp
    objects/mixer/src/spluginchain.cpp
    objects/mixer/src/sremoveassetaction.cpp
    objects/mixer/src/sremoveassetplacementaction.cpp
    objects/mixer/src/sremovepluginaction.cpp
    objects/mixer/src/sstdmixer.cpp
    # --- app/actions
    actions/include/app/actions/saction.h
    actions/include/app/actions/sactionhistory.h
    actions/include/app/actions/sactionqueue.h
    actions/include/app/actions/sactionregistry.h
    actions/include/app/actions/sactionundocommand.h
    actions/include/app/actions/scycleaction.h
    actions/include/app/actions/sgridaction.h
    actions/include/app/actions/smetronomeaction.h
    actions/include/app/actions/srenderaction.h
    actions/include/app/actions/ssetpropertyaction.h
    actions/include/app/actions/ssnaptogridaction.h
    actions/include/app/actions/stoggleplaybackaction.h
    actions/include/app/actions/stogglesettingaction.h
    actions/src/sactionhistory.cpp
    actions/src/sactionqueue.cpp
    actions/src/sactionregistry.cpp
    actions/src/sactionundocommand.cpp
    actions/src/scycleaction.cpp
    actions/src/sgridaction.cpp
    actions/src/smetronomeaction.cpp
    actions/src/srenderaction.cpp
    actions/src/ssetpropertyaction.cpp
    actions/src/ssnaptogridaction.cpp
    actions/src/stoggleplaybackaction.cpp
    # --- app/persistence
    persistence/include/app/persistence/sloadprojectaction.h
    persistence/src/sloadprojectaction.cpp
    persistence/include/app/persistence/sprojectloader.h
    persistence/include/app/persistence/ssaveprojectaction.h
    persistence/src/sprojectloader.cpp
    persistence/src/ssaveprojectaction.cpp
    # --- app/selection
    selection/include/app/selection/saddtoselectionaction.h
    selection/include/app/selection/sclearselectionaction.h
    selection/include/app/selection/sremovefromselectionaction.h
    selection/include/app/selection/sselectionmanager.h
    selection/include/app/selection/ssetselectionaction.h
    selection/include/app/selection/stoggleselectionaction.h
    selection/src/saddtoselectionaction.cpp
    selection/src/sclearselectionaction.cpp
    selection/src/sremovefromselectionaction.cpp
    selection/src/sselectionmanager.cpp
    selection/src/ssetselectionaction.cpp
    selection/src/stoggleselectionaction.cpp
    # --- app/timeline
    timeline/include/app/timeline/sgridtoolbar.h
    timeline/include/app/timeline/sobjecttreeview.h
    timeline/include/app/timeline/ssmvmixercontrol.h
    timeline/include/app/timeline/sstdmixerview.h
    timeline/include/app/timeline/strackdetailpanel.h
    timeline/include/app/timeline/strackheaderresizer.h
    timeline/include/app/timeline/szoomscrollbar.h
    timeline/src/sgridtoolbar.cpp
    timeline/src/sobjecttreeview.cpp
    timeline/src/ssmvmixercontrol.cpp
    timeline/src/sstdmixerview.cpp
    timeline/src/strackdetailpanel.cpp
    timeline/src/strackheaderresizer.cpp
    timeline/src/szoomscrollbar.cpp
    # --- app/pluginui
    pluginui/include/app/pluginui/spluginbrowserdialog.h
    pluginui/include/app/pluginui/splugineffectstrip.h
    pluginui/include/app/pluginui/spluginparamereditor.h
    pluginui/src/spluginbrowserdialog.cpp
    pluginui/src/splugineffectstrip.cpp
    pluginui/src/spluginparamereditor.cpp
    # --- app/servicesui
    servicesui/include/app/servicesui/soptions.h
    servicesui/include/app/servicesui/soptionsdialog.h
    servicesui/include/app/servicesui/srecordingprogress.h
    servicesui/include/app/servicesui/srenderdialog.h
    servicesui/include/app/servicesui/srenderprogress.h
    servicesui/src/soptions.cpp
    servicesui/src/soptionsdialog.cpp
    servicesui/src/srecordingprogress.cpp
    servicesui/src/srenderdialog.cpp
    servicesui/src/srenderprogress.cpp
    # --- app/shell
    shell/include/app/shell/sapplication.h
    shell/include/app/shell/smainwindow.h
    shell/include/app/shell/ssettings.h
    shell/src/sapplication.cpp
    shell/src/smainwindow.cpp
    shell/src/ssettings.cpp
    # --- app/testkit
    testkit/include/app/testkit/sactionrunner.h
    testkit/include/app/testkit/sactionscript.h
    testkit/include/app/testkit/sassertaudioenergyaction.h
    testkit/include/app/testkit/sassertaudiopeakaction.h
    testkit/include/app/testkit/sscreenshotaction.h
    testkit/src/sactionrunner.cpp
    testkit/src/sactionscript.cpp
    testkit/src/sassertaudioenergyaction.cpp
    testkit/src/sassertaudiopeakaction.cpp
    testkit/src/sscreenshotaction.cpp
)

add_library(smaragd_app OBJECT ${SMARAGD_APP_FILES})

target_include_directories(smaragd_app PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/model/include
    ${CMAKE_CURRENT_SOURCE_DIR}/objects/cut/include
    ${CMAKE_CURRENT_SOURCE_DIR}/objects/wave/include
    ${CMAKE_CURRENT_SOURCE_DIR}/objects/track/include
    ${CMAKE_CURRENT_SOURCE_DIR}/objects/mixer/include
    ${CMAKE_CURRENT_SOURCE_DIR}/actions/include
    ${CMAKE_CURRENT_SOURCE_DIR}/persistence/include
    ${CMAKE_CURRENT_SOURCE_DIR}/selection/include
    ${CMAKE_CURRENT_SOURCE_DIR}/timeline/include
    ${CMAKE_CURRENT_SOURCE_DIR}/pluginui/include
    ${CMAKE_CURRENT_SOURCE_DIR}/servicesui/include
    ${CMAKE_CURRENT_SOURCE_DIR}/shell/include
    ${CMAKE_CURRENT_SOURCE_DIR}/testkit/include
)

# XPM icon includes ("pix/zoomin.xpm") resolve from the smaragd/ source root.
target_include_directories(smaragd_app PRIVATE ${CMAKE_SOURCE_DIR})

target_link_libraries(smaragd_app
    PUBLIC
        tw303a
        Qt${QT_VERSION_MAJOR}::Core
        Qt${QT_VERSION_MAJOR}::Widgets
        Qt${QT_VERSION_MAJOR}::Xml
)

# Optionally embed a bundled UI font. Drop a TTF at main/resources/FreeSans.ttf
# and re-run CMake: it is compiled into the binary (AUTORCC) and used as the
# default, antialiased UI font (see main.cpp). If absent, the platform default
# font is used and the build is unaffected. (GNU FreeFont / FreeSans is GPL with
# a font exception, so bundling it in the application is fine.)
set(SMARAGD_MAIN_SOURCES shell/src/main.cpp)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/resources/FreeSans.ttf)
    list(APPEND SMARAGD_MAIN_SOURCES resources/fonts.qrc)
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
    add_executable(smaragd WIN32 ${SMARAGD_MAIN_SOURCES})
elseif(SMARAGD_MACOS)
    add_executable(smaragd MACOSX_BUNDLE ${SMARAGD_MAIN_SOURCES})

    # Code-sign app with microphone entitlements after building
    add_custom_command(TARGET smaragd POST_BUILD
        COMMAND codesign --force --deep --sign -
                --entitlements "${CMAKE_CURRENT_SOURCE_DIR}/smaragd.entitlements"
                "$<TARGET_BUNDLE_DIR:smaragd>"
        COMMENT "Code-signing smaragd with microphone entitlements"
    )
else()
    add_executable(smaragd ${SMARAGD_MAIN_SOURCES})
endif()

if(SMARAGD_MACOS)
    set_target_properties(smaragd PROPERTIES
        MACOSX_BUNDLE_GUI_IDENTIFIER "dev.tweggen.smaragd"
        MACOSX_BUNDLE_BUNDLE_NAME "Smaragd"
        MACOSX_BUNDLE_BUNDLE_VERSION "1.0.0"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0"
    )
endif()

target_link_libraries(smaragd
    PRIVATE
        smaragd_app
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
        if(SMARAGD_MACOS)
            # macdeployqt expects the .app bundle directory, not the executable
            set(_DEPLOY_TARGET "$<TARGET_BUNDLE_DIR:smaragd>")
        else()
            # windeployqt expects the executable path
            set(_DEPLOY_TARGET "$<TARGET_FILE:smaragd>")
        endif()

        add_custom_command(TARGET smaragd POST_BUILD
            COMMAND "${DEPLOYQT_EXECUTABLE}" ${DEPLOYQT_ARGS} "${_DEPLOY_TARGET}"
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

# vcpkg render deps (libsndfile/libvorbis/libogg and their codec DLLs) are
# dynamic on the x64-mingw-dynamic triplet; copy them next to the exe so the
# binary stays self-contained (windeployqt handles only the Qt DLLs). Without
# this the exe silently fails to start after a clean build.
if(SMARAGD_WINDOWS AND DEFINED VCPKG_INSTALLED_DIR)
    file(GLOB _VCPKG_RUNTIME_DLLS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/*.dll")
    if(_VCPKG_RUNTIME_DLLS)
        add_custom_command(TARGET smaragd POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_VCPKG_RUNTIME_DLLS} "$<TARGET_FILE_DIR:smaragd>"
            COMMENT "Copying vcpkg runtime DLLs next to smaragd.exe"
            VERBATIM
        )
    endif()
endif()

# Action round-trip test (Phase 0 of action scripts): verify all actions
# can serialize and deserialize without loss. Links the app object library —
# no more recompiling every app source into the test binary.
add_executable(action_roundtrip_test
    testkit/src/action_roundtrip_test.cpp
)

target_link_libraries(action_roundtrip_test
    PRIVATE
        smaragd_app
)
