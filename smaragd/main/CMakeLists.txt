# Smaragd application (proposal 14). The app is LAYERED (Phase 6):
#
#     app_model < app_core < app_objects < app_ui
#
# and since this file, the layers are COMPILE-TIME ENFORCED: each layer is
# an OBJECT library that publishes only its own modules' include dirs and
# links only the lower layers plus its declared engine modules — a
# cross-layer include (model -> actions, core -> objects, anything below the
# UI -> shell) fails to compile. Intra-layer module edges are still guarded
# by tools/check_layering.py.
#
# OBJECT (not STATIC) libraries are load-bearing: the actions and the
# loader/editor/extern-file registries self-register via static
# initializers, which STATIC libraries would silently drop at link time.
# Consequence: the executables must link ALL FOUR layer targets directly
# (object files do not propagate transitively through object libraries).

set(APP_MODEL_FILES
    # --- app/model
    model/include/app/model/sappcontext.h
    model/include/app/model/sdetaileditors.h
    model/include/app/model/sexternfile.h
    model/include/app/model/sexternfilelist.h
    model/include/app/model/sfilepathref.h
    model/include/app/model/slink.h
    model/include/app/model/sobject.h
    model/include/app/model/sobjectpath.h
    model/include/app/model/sobjectrenderer.h
    model/include/app/model/splacements.h
    model/include/app/model/sproject.h
    model/include/app/model/seditgroups.h
    model/include/app/model/sprojectprops.h
    model/include/app/model/ssolorules.h
    model/include/app/model/ssortedobjlist.h
    model/src/sappcontext.cpp
    model/src/sdetaileditors.cpp
    model/src/sexternfile.cpp
    model/src/sexternfilelist.cpp
    model/src/sfilepathref.cpp
    model/src/slink.cpp
    model/src/sobject.cpp
    model/src/sobjectrenderer.cpp
    model/src/sproject.cpp
)

set(APP_CORE_FILES
    # --- app/actions
    actions/include/app/actions/saction.h
    actions/include/app/actions/sactionhistory.h
    actions/include/app/actions/sactionqueue.h
    actions/include/app/actions/sactionregistry.h
    actions/include/app/actions/sactionundocommand.h
    actions/include/app/actions/scompositeaction.h
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
    actions/src/scompositeaction.cpp
    actions/src/scycleaction.cpp
    actions/src/sgridaction.cpp
    actions/src/smetronomeaction.cpp
    actions/src/srenderaction.cpp
    actions/src/ssetpropertyaction.cpp
    actions/src/ssnaptogridaction.cpp
    actions/src/stoggleplaybackaction.cpp
    # --- app/persistence
    persistence/include/app/persistence/sloadprojectaction.h
    persistence/include/app/persistence/sprojectloader.h
    persistence/include/app/persistence/ssaveprojectaction.h
    persistence/src/sloadprojectaction.cpp
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
)

set(APP_OBJECTS_FILES
    # --- app/objects/cut
    objects/cut/include/app/objects/cut/saddsampleaction.h
    objects/cut/include/app/objects/cut/scut.h
    objects/cut/include/app/objects/cut/scutrndrinline.h
    objects/cut/include/app/objects/cut/sduplicateclipaction.h
    objects/cut/include/app/objects/cut/sremoveclipaction.h
    objects/cut/include/app/objects/cut/sremovesampleaction.h
    objects/cut/include/app/objects/cut/srestorecontainerclipaction.h
    objects/cut/include/app/objects/cut/sresizeclipaction.h
    objects/cut/include/app/objects/cut/saddtakeaction.h
    objects/cut/include/app/objects/cut/sremovetakeaction.h
    objects/cut/include/app/objects/cut/sselecttakeaction.h
    objects/cut/include/app/objects/cut/ssetclipnameaction.h
    objects/cut/include/app/objects/cut/ssetpitchaction.h
    objects/cut/include/app/objects/cut/ssetformantpreserveaction.h
    objects/cut/include/app/objects/cut/splaceclipaction.h
    objects/cut/include/app/objects/cut/splacerecordingaction.h
    objects/cut/include/app/objects/cut/ssplitclipaction.h
    objects/cut/include/app/objects/cut/stakehelpers.h
    objects/cut/include/app/objects/cut/stakestack.h
    objects/cut/include/app/objects/cut/sunsplitclipaction.h
    objects/cut/include/app/objects/cut/swarpmarkeractions.h
    objects/cut/src/saddsampleaction.cpp
    objects/cut/src/saddtakeaction.cpp
    objects/cut/src/scut.cpp
    objects/cut/src/scutrndrinline.cpp
    objects/cut/src/sduplicateclipaction.cpp
    objects/cut/src/sremoveclipaction.cpp
    objects/cut/src/sremovesampleaction.cpp
    objects/cut/src/srestorecontainerclipaction.cpp
    objects/cut/src/sremovetakeaction.cpp
    objects/cut/src/sresizeclipaction.cpp
    objects/cut/src/sselecttakeaction.cpp
    objects/cut/src/ssetclipnameaction.cpp
    objects/cut/src/ssetpitchaction.cpp
    objects/cut/src/ssetformantpreserveaction.cpp
    objects/cut/src/splaceclipaction.cpp
    objects/cut/src/splacerecordingaction.cpp
    objects/cut/src/ssplitclipaction.cpp
    objects/cut/src/stakehelpers.cpp
    objects/cut/src/stakestack.cpp
    objects/cut/src/sunsplitclipaction.cpp
    objects/cut/src/swarpmarkeractions.cpp
    # --- app/objects/wave
    objects/wave/include/app/objects/wave/splainwave.h
    objects/wave/include/app/objects/wave/splainwaverndrinline.h
    objects/wave/include/app/objects/wave/swaveformdraw.h
    objects/wave/src/splainwave.cpp
    objects/wave/src/splainwaverndrinline.cpp
    objects/wave/src/swaveformdraw.cpp
    # --- app/objects/track
    objects/track/include/app/objects/track/sinsertpluginaction.h
    objects/track/include/app/objects/track/seteditgroupaction.h
    objects/track/include/app/objects/track/smoveclipaction.h
    objects/track/include/app/objects/track/spluginactionsupport.h
    objects/track/include/app/objects/track/spluginchain.h
    objects/track/include/app/objects/track/spluginslot.h
    objects/track/include/app/objects/track/sremovepluginaction.h
    objects/track/include/app/objects/track/sreorderpluginaction.h
    objects/track/include/app/objects/track/ssetpluginbypassaction.h
    objects/track/include/app/objects/track/ssetpluginparamaction.h
    objects/track/include/app/objects/track/ssettrackmuteaction.h
    objects/track/include/app/objects/track/ssettracksoloaction.h
    objects/track/include/app/objects/track/ssettrackvolumeaction.h
    objects/track/include/app/objects/track/strack.h
    objects/track/include/app/objects/track/strackcolormodifier.h
    objects/track/include/app/objects/track/strackpath.h
    objects/track/include/app/objects/track/strackrenderer.h
    objects/track/include/app/objects/track/strackrndrinline.h
    objects/track/src/seteditgroupaction.cpp
    objects/track/src/sinsertpluginaction.cpp
    objects/track/src/smoveclipaction.cpp
    objects/track/src/spluginchain.cpp
    objects/track/src/spluginslot.cpp
    objects/track/src/sremovepluginaction.cpp
    objects/track/src/sreorderpluginaction.cpp
    objects/track/src/ssetpluginbypassaction.cpp
    objects/track/src/ssetpluginparamaction.cpp
    objects/track/src/ssettrackmuteaction.cpp
    objects/track/src/ssettracksoloaction.cpp
    objects/track/src/ssettrackvolumeaction.cpp
    objects/track/src/strack.cpp
    objects/track/src/strackcolormodifier.cpp
    objects/track/src/strackrndrinline.cpp
    # --- app/objects/mixer
    objects/mixer/include/app/objects/mixer/saddtrackaction.h
    objects/mixer/include/app/objects/mixer/screateassetaction.h
    objects/mixer/include/app/objects/mixer/smovetrackaction.h
    objects/mixer/include/app/objects/mixer/splaceassetaction.h
    objects/mixer/include/app/objects/mixer/sremoveassetaction.h
    objects/mixer/include/app/objects/mixer/sremoveassetplacementaction.h
    objects/mixer/include/app/objects/mixer/sremovetrackaction.h
    objects/mixer/include/app/objects/mixer/sreparenttrackaction.h
    objects/mixer/include/app/objects/mixer/srestoretrackaction.h
    objects/mixer/include/app/objects/mixer/sstdmixer.h
    objects/mixer/src/saddtrackaction.cpp
    objects/mixer/src/screateassetaction.cpp
    objects/mixer/src/smovetrackaction.cpp
    objects/mixer/src/splaceassetaction.cpp
    objects/mixer/src/sremoveassetaction.cpp
    objects/mixer/src/sremoveassetplacementaction.cpp
    objects/mixer/src/sremovetrackaction.cpp
    objects/mixer/src/sreparenttrackaction.cpp
    objects/mixer/src/srestoretrackaction.cpp
    objects/mixer/src/sstdmixer.cpp
)

set(APP_UI_FILES
    # --- app/timeline
    timeline/include/app/timeline/sclippropertiespanel.h
    timeline/include/app/timeline/sgridtoolbar.h
    timeline/include/app/timeline/sobjecttreeview.h
    timeline/include/app/timeline/ssmvmixercontrol.h
    timeline/include/app/timeline/sstdmixerview.h
    timeline/include/app/timeline/strackdetailpanel.h
    timeline/include/app/timeline/sfadercurve.h
    timeline/include/app/timeline/slevelmeter.h
    timeline/include/app/timeline/strackheaderresizer.h
    timeline/include/app/timeline/szoomscrollbar.h
    timeline/src/sclippropertiespanel.cpp
    timeline/src/sgridtoolbar.cpp
    timeline/src/sobjecttreeview.cpp
    timeline/src/ssmvmixercontrol.cpp
    timeline/src/sstdmixerview.cpp
    timeline/src/strackdetailpanel.cpp
    timeline/src/slevelmeter.cpp
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
    servicesui/include/app/servicesui/slogmodel.h
    servicesui/include/app/servicesui/slogview.h
    servicesui/include/app/servicesui/soptions.h
    servicesui/include/app/servicesui/soptionsdialog.h
    servicesui/include/app/servicesui/srecordingprogress.h
    servicesui/include/app/servicesui/srenderdialog.h
    servicesui/include/app/servicesui/srenderprogress.h
    servicesui/src/slogmodel.cpp
    servicesui/src/slogview.cpp
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
    testkit/include/app/testkit/sassertaudiofrequencyaction.h
    testkit/include/app/testkit/sassertaudiopeakaction.h
    testkit/include/app/testkit/sassertfilecontainsaction.h
    testkit/include/app/testkit/sassertsourcepositionaction.h
    testkit/include/app/testkit/sdragclipedgeaction.h
    testkit/include/app/testkit/sdumpplaybackcaptureaction.h
    testkit/include/app/testkit/sgrouptrackaction.h
    testkit/include/app/testkit/slanelayouttestactions.h
    testkit/include/app/testkit/slogstressaction.h
    testkit/include/app/testkit/smetertestactions.h
    testkit/include/app/testkit/spluginuitestactions.h
    testkit/include/app/testkit/sscreenshotaction.h
    testkit/include/app/testkit/ssidecartestactions.h
    testkit/include/app/testkit/sslipclipaction.h
    testkit/include/app/testkit/strackselectionactions.h
    testkit/include/app/testkit/stransporttestactions.h
    testkit/src/sactionrunner.cpp
    testkit/src/sactionscript.cpp
    testkit/src/sassertaudioenergyaction.cpp
    testkit/src/sassertaudiofrequencyaction.cpp
    testkit/src/sassertaudiopeakaction.cpp
    testkit/src/sassertfilecontainsaction.cpp
    testkit/src/sassertsourcepositionaction.cpp
    testkit/src/sdragclipedgeaction.cpp
    testkit/src/sdumpplaybackcaptureaction.cpp
    testkit/src/sgrouptrackaction.cpp
    testkit/src/slanelayouttestactions.cpp
    testkit/src/slogstressaction.cpp
    testkit/src/smetertestactions.cpp
    testkit/src/spluginuitestactions.cpp
    testkit/src/sscreenshotaction.cpp
    testkit/src/ssidecartestactions.cpp
    testkit/src/sslipclipaction.cpp
    testkit/src/strackselectionactions.cpp
    testkit/src/stransporttestactions.cpp
)

# Layer 1 — the document model (+ the Phase 6 seams: SAppContext,
# sdetaileditors, sobjectpath). Engine access: exactly APP_ENG(model).
add_library(app_model OBJECT ${APP_MODEL_FILES})
target_include_directories(app_model PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/model/include)
target_link_libraries(app_model
    PUBLIC
        tw_core tw_graph tw_pages tw_schedule tw_sources
        Qt${QT_VERSION_MAJOR}::Core
        Qt${QT_VERSION_MAJOR}::Gui
        Qt${QT_VERSION_MAJOR}::Widgets
        Qt${QT_VERSION_MAJOR}::Xml
)

# Layer 2 — command framework, persistence, selection.
add_library(app_core OBJECT ${APP_CORE_FILES})
target_include_directories(app_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/actions/include
    ${CMAKE_CURRENT_SOURCE_DIR}/persistence/include
    ${CMAKE_CURRENT_SOURCE_DIR}/selection/include
)
target_link_libraries(app_core PUBLIC app_model tw_render)

# Layer 3 — the object slices (cut, wave, track, mixer).
add_library(app_objects OBJECT ${APP_OBJECTS_FILES})
target_include_directories(app_objects PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/objects/cut/include
    ${CMAKE_CURRENT_SOURCE_DIR}/objects/wave/include
    ${CMAKE_CURRENT_SOURCE_DIR}/objects/track/include
    ${CMAKE_CURRENT_SOURCE_DIR}/objects/mixer/include
)
target_link_libraries(app_objects PUBLIC app_core tw_mix tw_plugins tw_sidecar)

# Layer 4 — the UI and the shell (the only layer that may see everything).
add_library(app_ui OBJECT ${APP_UI_FILES})
target_include_directories(app_ui PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/timeline/include
    ${CMAKE_CURRENT_SOURCE_DIR}/pluginui/include
    ${CMAKE_CURRENT_SOURCE_DIR}/servicesui/include
    ${CMAKE_CURRENT_SOURCE_DIR}/shell/include
    ${CMAKE_CURRENT_SOURCE_DIR}/testkit/include
)
# XPM icon includes ("pix/zoomin.xpm") resolve from the smaragd/ source root.
target_include_directories(app_ui PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(app_ui PUBLIC app_objects tw303a)


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

# SMARAGD_LOG_CONSOLE_DEFAULT (proposal 24): whether TwLog tees to stderr when
# the user has expressed no preference. ORTHOGONAL to SMARAGD_WIN_CONSOLE, which
# only decides whether Windows gives the process a console to write to. Defaults
# to ON for a Debug build and OFF otherwise — "console by default in debug, by
# option in release". The persisted SOpt::LogConsole, then SMARAGD_LOG_CONSOLE in
# the environment, then --log-console/--no-log-console each override it.
if(CMAKE_BUILD_TYPE STREQUAL "Debug" OR NOT CMAKE_BUILD_TYPE)
    set(SMARAGD_LOG_CONSOLE_DEFAULT_INIT ON)
else()
    set(SMARAGD_LOG_CONSOLE_DEFAULT_INIT OFF)
endif()
option(SMARAGD_LOG_CONSOLE_DEFAULT
    "Tee the log to stderr unless overridden (default: ON for Debug builds)"
    ${SMARAGD_LOG_CONSOLE_DEFAULT_INIT})
message(STATUS "Smaragd: log console default is ${SMARAGD_LOG_CONSOLE_DEFAULT}")

# PUBLIC so the smaragd executable (main.cpp) inherits it along with app_ui.
target_compile_definitions(app_ui PUBLIC
    SMARAGD_LOG_CONSOLE_DEFAULT=$<BOOL:${SMARAGD_LOG_CONSOLE_DEFAULT}>)

if(SMARAGD_WINDOWS AND NOT SMARAGD_WIN_CONSOLE)
    add_executable(smaragd WIN32 ${SMARAGD_MAIN_SOURCES})
elseif(SMARAGD_MACOS)
    add_executable(smaragd MACOSX_BUNDLE ${SMARAGD_MAIN_SOURCES})

    # The out-of-process plugin probe has to live INSIDE the bundle: the app
    # locates it as applicationDirPath()/smaragd_pluginprobe, which on macOS is
    # Contents/MacOS. Added BEFORE the codesign command below on purpose —
    # POST_BUILD steps run in the order they are declared, and dropping a file
    # into the bundle after --deep --sign would leave the sealed resources stale.
    if(TARGET smaragd_pluginprobe)
        add_custom_command(TARGET smaragd POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:smaragd_pluginprobe>"
                    "$<TARGET_BUNDLE_DIR:smaragd>/Contents/MacOS/"
            COMMENT "Copying smaragd_pluginprobe into the app bundle"
        )
    endif()

    # The twtestclap fixture is resolved by qxa plugin cases through a RELATIVE
    # path against QCoreApplication::applicationDirPath() (SPluginSlot::
    # resolveModulePath). On macOS that directory is Contents/MacOS INSIDE the
    # bundle, whereas tw303a drops the fixture next to the .app (build/bin) — so
    # it must be copied in here as well, or every plugin qxa case loads a
    # transparent Missing placeholder and its audio assertions fail. Same
    # before-codesign ordering rule as the probe above. Flat .clap dylib: the
    # macOS loader now dlopens a plain-file .clap directly (twclapmodule.cc).
    if(TARGET twtestclap)
        add_custom_command(TARGET smaragd POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:twtestclap>"
                    "$<TARGET_BUNDLE_DIR:smaragd>/Contents/MacOS/twtestclap.clap"
            COMMENT "Copying the twtestclap fixture into the app bundle (qxa cases)"
        )
    endif()

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
        app_ui app_objects app_core app_model
)

# Not a link dependency — a SEPARATE executable the app spawns at scan time
# (proposal 08 M2). Declared so building just the `smaragd` target still produces
# it; without it the app silently falls back to in-process probing, i.e. loses
# its crash isolation, which is the kind of regression nobody notices.
if(TARGET smaragd_pluginprobe)
    add_dependencies(smaragd smaragd_pluginprobe)
endif()

# The macOS POST_BUILD above copies the twtestclap fixture into the bundle before
# codesigning; make it exist first. Only relevant on macOS (the copy is gated by
# SMARAGD_MACOS), harmless elsewhere.
if(SMARAGD_MACOS AND TARGET twtestclap)
    add_dependencies(smaragd twtestclap)
endif()

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

        # windeployqt deploys only the platform plugin the APP needs — qwindows.
        # But every headless gate (the ~90 qxa cases via --test-case, plus
        # action_roundtrip_test / filepathref_test / preview_container_test) runs
        # with QT_QPA_PLATFORM=offscreen, and qoffscreen.dll is NOT deployed.
        # Its absence does not fail fast: Qt blocks in a "this application failed
        # to start" platform-plugin dialog that nobody can see in a headless run,
        # so each affected test burns its ENTIRE timeout (300 s) at ~0 s of CPU
        # instead of passing in ~0.05 s. That has cost real gate runs ten
        # minutes of pure timeout and reads as a suite problem rather than a
        # missing file, so deploy it here instead of leaving it to be
        # rediscovered.
        if(TARGET Qt${QT_VERSION_MAJOR}::QOffscreenIntegrationPlugin)
            add_custom_command(TARGET smaragd POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory
                        "$<TARGET_FILE_DIR:smaragd>/platforms"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "$<TARGET_FILE:Qt${QT_VERSION_MAJOR}::QOffscreenIntegrationPlugin>"
                        "$<TARGET_FILE_DIR:smaragd>/platforms/"
                COMMENT "Deploying the offscreen platform plugin (headless tests)"
                VERBATIM
            )
        else()
            message(WARNING
                "Qt${QT_VERSION_MAJOR}::QOffscreenIntegrationPlugin was not found, so "
                "qoffscreen.dll cannot be deployed. Headless tests that set "
                "QT_QPA_PLATFORM=offscreen will HANG until their timeout rather "
                "than fail fast. Copy it next to the binary by hand: "
                "<QtPrefix>/plugins/platforms/qoffscreen.* -> "
                "build/bin/platforms/.")
        endif()
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
    # Rubber Band was removed 2026-07-26 (GPL relicensing) — its DLL may
    # still sit in the shared vcpkg install for other projects, but it must
    # not be deployed next to a binary that no longer links it.
    list(FILTER _VCPKG_RUNTIME_DLLS EXCLUDE REGEX "rubberband")
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
        app_ui app_objects app_core app_model
)

# It was BUILT but never registered, so `ctest` never ran it: three milestones
# of new action verbs went in without the round-trip audit ever gating anything
# (proposal 08 M5). It needs a QApplication for the widget-owning verbs' static
# registrars, and offscreen so it never touches a display.
add_test(NAME action_roundtrip_test COMMAND action_roundtrip_test)
set_tests_properties(action_roundtrip_test PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# External-file path encoding (project-relative / "~" / absolute). Pure path
# arithmetic over app_model, so it needs no project, no audio and no display —
# but it links all four layers because app_model is an OBJECT library whose
# static registrars pull in the rest.
add_executable(filepathref_test testkit/src/filepathref_test.cpp)
target_link_libraries(filepathref_test
    PRIVATE
        app_ui app_objects app_core app_model
)
add_test(NAME filepathref_test COMMAND filepathref_test)
set_tests_properties(filepathref_test PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# The CONTAINER branch of SObject::straightCalcPreviewData(): a preview built
# from a live sub-arrangement reads FROZEN PAGES (chained, page-aligned) rather
# than seeking the live component per preview window. Values, page-boundary
# indexing, state chaining and the reposition count are all asserted against a
# synthetic component — the paint path is otherwise the only consumer, and a
# screenshot cannot localise any of them. Needs a QApplication (offscreen) for
# the app object libraries' static registrars, like the two tests above.
add_executable(preview_container_test testkit/src/preview_container_test.cpp)
target_link_libraries(preview_container_test
    PRIVATE
        app_ui app_objects app_core app_model
)
add_test(NAME preview_container_test COMMAND preview_container_test)
set_tests_properties(preview_container_test PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
