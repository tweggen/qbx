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
    include/ssmvmixercontrol.h
    include/ssortedobjlist.h
    include/sstdmixer.h
    include/sstdmixerview.h
    include/strack.h
    include/strackrenderer.h
    include/strackrndrinline.h
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
    src/ssmvmixercontrol.cpp
    src/sstdmixer.cpp
    src/sstdmixerview.cpp
    src/strack.cpp
    src/strackrndrinline.cpp
)

if(SMARAGD_WINDOWS)
    add_executable(smaragd WIN32 ${SMARAGD_SOURCES} ${SMARAGD_HEADERS})
elseif(SMARAGD_MACOS)
    add_executable(smaragd MACOSX_BUNDLE ${SMARAGD_SOURCES} ${SMARAGD_HEADERS})
else()
    add_executable(smaragd ${SMARAGD_SOURCES} ${SMARAGD_HEADERS})
endif()

target_include_directories(smaragd
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(smaragd
    PRIVATE
        tw303a
        Qt${QT_VERSION_MAJOR}::Core
        Qt${QT_VERSION_MAJOR}::Widgets
        Qt${QT_VERSION_MAJOR}::Xml
)
