cmake_minimum_required(VERSION 3.22)

project(MyCalibGui VERSION 0.4.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

option(MYCALIB_ENABLE_LTO "Enable link-time optimization" OFF)
if(MYCALIB_ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT ipo_supported OUTPUT ipo_output)
    if(ipo_supported)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
    else()
        message(WARNING "IPO/LTO is not supported: ${ipo_output}")
    endif()
endif()

find_package(Qt6 6.4 COMPONENTS Widgets Concurrent Svg REQUIRED)
find_package(OpenCV 4.5 REQUIRED COMPONENTS calib3d imgproc highgui features2d)

include(GNUInstallDirs)

set(MYCALIB_SOURCES
    src/main.cpp
    src/MainWindow.cpp
    src/CalibrationEngine.cpp
    src/BoardDetector.cpp
    src/HeatmapGenerator.cpp
    src/ImageLoader.cpp
    src/Logger.cpp
    src/HeatmapView.cpp
    src/DetectionPreviewWidget.cpp
    src/ResidualScatterView.cpp
    src/Pose3DView.cpp
    src/ParameterDialog.cpp
    src/ImageEvaluationDialog.cpp
    src/PaperFigureExporter.cpp
)

set(MYCALIB_HEADERS
    include/MainWindow.h
    include/CalibrationEngine.h
    include/HeatmapGenerator.h
    include/ImageLoader.h
    include/BoardSpec.h
    include/BoardDetector.h
    include/DetectionResult.h
    include/Logger.h
    include/HeatmapView.h
    include/DetectionPreviewWidget.h
    include/ResidualScatterView.h
    include/Pose3DView.h
    include/ParameterDialog.h
    include/ImageEvaluationDialog.h
    include/PaperFigureExporter.h
)

set(MYCALIB_RESOURCES
    src/resources.qrc
)

add_executable(my_calib_gui
    ${MYCALIB_SOURCES}
    ${MYCALIB_HEADERS}
    ${MYCALIB_RESOURCES}
)

set(APP_ICON_PNG "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/app_icon.png")
if(APPLE)
    set(APP_ICON_MAC "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/app_icon.icns")
    if(EXISTS "${APP_ICON_MAC}")
        set_source_files_properties(${APP_ICON_MAC} PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
        target_sources(my_calib_gui PRIVATE ${APP_ICON_MAC})
        set_target_properties(my_calib_gui PROPERTIES MACOSX_BUNDLE_ICON_FILE "app_icon.icns")
    else()
        message(WARNING "macOS bundle icon not found at ${APP_ICON_MAC}. The app will use the default icon.")
    endif()
elseif(WIN32)
    set(APP_ICON_WIN "${CMAKE_CURRENT_SOURCE_DIR}/src/my_calib.rc")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/app_icon.ico")
        target_sources(my_calib_gui PRIVATE ${APP_ICON_WIN})
    else()
        message(WARNING "Windows icon not found at resources/icons/app_icon.ico. The app will use the default icon.")
    endif()
endif()

set_target_properties(my_calib_gui PROPERTIES
    WIN32_EXECUTABLE ON
)

if(APPLE)
    set_target_properties(my_calib_gui PROPERTIES
        MACOSX_BUNDLE ON
        MACOSX_BUNDLE_BUNDLE_NAME "Calib Evaluator"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.charmingzh.calib"
        MACOSX_BUNDLE_INFO_STRING "Calib Evaluator"
        MACOSX_BUNDLE_LONG_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    )
    set(CMAKE_INSTALL_RPATH "@executable_path/../Frameworks")
endif()

target_include_directories(my_calib_gui PRIVATE include)

target_link_libraries(my_calib_gui PRIVATE
    Qt6::Widgets
    Qt6::Concurrent
    Qt6::Svg
    ${OpenCV_LIBS}
)

target_compile_definitions(my_calib_gui PRIVATE QT_NO_KEYWORDS)

if(MSVC)
    target_compile_options(my_calib_gui PRIVATE /W4 /permissive- /Zc:__cplusplus)
else()
    target_compile_options(my_calib_gui PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(APPLE)
    install(TARGETS my_calib_gui BUNDLE DESTINATION .)
elseif(WIN32)
    install(TARGETS my_calib_gui RUNTIME DESTINATION .)
else()
    install(TARGETS my_calib_gui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

set(CPACK_PACKAGE_NAME "Calib Evaluator")
set(CPACK_PACKAGE_VENDOR "CharmingZh")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Interactive calibration evaluation tool")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_CONTACT "calib@charmingzh.dev")

if(APPLE)
    set(CPACK_GENERATOR "DragNDrop")
    set(CPACK_DMG_VOLUME_NAME "Calib Evaluator")
elseif(WIN32)
    set(CPACK_GENERATOR "NSIS" "ZIP")
    set(CPACK_NSIS_DISPLAY_NAME "Calib Evaluator")
    set(CPACK_NSIS_PACKAGE_NAME "Calib Evaluator")
    set(CPACK_NSIS_INSTALL_ROOT "$PROGRAMFILES64")
    set(CPACK_NSIS_CONTACT "calib@charmingzh.dev")
    set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
else()
    set(CPACK_GENERATOR "TGZ")
endif()

include(CPack)
