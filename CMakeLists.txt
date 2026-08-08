cmake_minimum_required(VERSION 3.24)

# Only auto-load vcpkg when VCPKG_ROOT is a non-empty path. An empty
# VCPKG_ROOT (common when CI clears it) is still "defined" in CMake and would
# otherwise produce the invalid path "/scripts/buildsystems/vcpkg.cmake".
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE
   AND DEFINED ENV{VCPKG_ROOT}
   AND NOT "$ENV{VCPKG_ROOT}" STREQUAL "")
    set(
        CMAKE_TOOLCHAIN_FILE
        "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE FILEPATH
        "Default WellLogEngine vcpkg toolchain"
    )
endif()

project(
    WellLogEngine
    VERSION 0.1.0
    DESCRIPTION "Host-independent C++20 well logging engine"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)
include(CMakePackageConfigHelpers)
include(GNUInstallDirs)
include(FetchContent)
find_package(Threads REQUIRED)
# Prefer a system ZLIB; fall back to FetchContent so Windows runners without a
# system install still configure cleanly. When fetched, we also install the
# static archive so package.consumer's find_dependency(ZLIB) can resolve it.
set(WELLLOG_FETCHED_ZLIB OFF)
find_package(ZLIB QUIET)
if(NOT ZLIB_FOUND)
    message(STATUS "ZLIB not found; fetching madler/zlib 1.3.1 via FetchContent")
    set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(ZLIB_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        welllog_zlib
        URL https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz
        URL_HASH SHA256=9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(welllog_zlib)
    if(TARGET zlibstatic AND NOT TARGET ZLIB::ZLIB)
        add_library(ZLIB::ZLIB ALIAS zlibstatic)
        set(WELLLOG_ZLIB_INSTALL_TARGET zlibstatic)
    elseif(TARGET zlib AND NOT TARGET ZLIB::ZLIB)
        add_library(ZLIB::ZLIB ALIAS zlib)
        set(WELLLOG_ZLIB_INSTALL_TARGET zlib)
    endif()
    if(NOT TARGET ZLIB::ZLIB)
        message(FATAL_ERROR "Fetched zlib but no ZLIB::ZLIB target was created")
    endif()
    set(WELLLOG_FETCHED_ZLIB ON)
endif()

option(WELLLOG_BUILD_TESTS "Build WellLogEngine tests" ${BUILD_TESTING})
option(WELLLOG_BUILD_SHARED "Build WellLogEngine shared libraries" OFF)
option(WELLLOG_WARNINGS_AS_ERRORS "Treat WellLogEngine warnings as errors" OFF)
option(WELLLOG_BUILD_BENCHMARKS "Build WellLogEngine performance benchmarks" OFF)
option(
    WELLLOG_BUILD_QT_WIDGETS
    "Build the optional Qt Widgets/OpenGL adapter"
    OFF
)
option(
    WELLLOG_BUILD_PYTHON
    "Build the optional PySide6/Shiboken6 bindings"
    OFF
)
option(
    WELLLOG_BUILD_TEXT
    "Build the Unicode text pipeline (HarfBuzz/FreeType/ICU)"
    ON
)
option(
    WELLLOG_BUILD_ARROW
    "Build the optional Arrow C Data / mmap / IPC adapter (ADR 0027)"
    ON
)

set(WELLLOG_ARROW_HAS_IPC OFF)
if(WELLLOG_BUILD_ARROW)
    find_package(Arrow CONFIG QUIET)
    if(Arrow_FOUND AND TARGET Arrow::arrow_shared)
        set(WELLLOG_ARROW_HAS_IPC ON)
        message(STATUS "WellLog Arrow IPC: enabled (Arrow ${ARROW_VERSION})")
    else()
        message(STATUS "WellLog Arrow IPC: disabled (Arrow C++ not found; C Data + mmap only)")
    endif()
endif()

if(WELLLOG_BUILD_TEXT)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(HarfBuzz REQUIRED IMPORTED_TARGET harfbuzz)
    pkg_check_modules(FreeType REQUIRED IMPORTED_TARGET freetype2)
    pkg_check_modules(ICU_UC REQUIRED IMPORTED_TARGET icu-uc)
    pkg_check_modules(ICU_I18N REQUIRED IMPORTED_TARGET icu-i18n)
endif()

if(WELLLOG_BUILD_PYTHON)
    set(WELLLOG_BUILD_QT_WIDGETS ON)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

if(WELLLOG_BUILD_QT_WIDGETS)
    find_package(
        Qt6 6.8
        REQUIRED
        COMPONENTS Widgets OpenGLWidgets
    )
    if(WELLLOG_BUILD_TESTS)
        find_package(Qt6 6.8 REQUIRED COMPONENTS Test)
    endif()
    qt_standard_project_setup(REQUIRES 6.8)
endif()

if(WELLLOG_BUILD_BENCHMARKS AND NOT WELLLOG_BUILD_QT_WIDGETS)
    find_package(Qt6 6.8 REQUIRED COMPONENTS Gui)
endif()

if(WELLLOG_BUILD_PYTHON)
    find_package(
        Python 3.12
        REQUIRED
        COMPONENTS Interpreter Development
    )
    if(Python_VERSION VERSION_GREATER_EQUAL 3.14)
        message(FATAL_ERROR "WellLog Python bindings support CPython 3.12 and 3.13")
    endif()
    execute_process(
        COMMAND
            "${Python_EXECUTABLE}"
            -c
            "import pathlib, PySide6, shiboken6, shiboken6_generator; print(pathlib.Path(PySide6.__file__).parent / 'lib' / 'cmake'); print(pathlib.Path(shiboken6.__file__).parent / 'lib' / 'cmake'); print(pathlib.Path(shiboken6_generator.__file__).parent / 'lib' / 'cmake'); print(pathlib.Path(PySide6.__file__).parent.parent)"
        RESULT_VARIABLE WELLLOG_PYTHON_PROFILE_RESULT
        OUTPUT_VARIABLE WELLLOG_PYTHON_CMAKE_PATHS
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(NOT WELLLOG_PYTHON_PROFILE_RESULT EQUAL 0)
        message(
            FATAL_ERROR
            "PySide6, shiboken6 and shiboken6_generator must be installed for the selected Python"
        )
    endif()
    string(REPLACE "\n" ";" WELLLOG_PYTHON_CMAKE_PATHS "${WELLLOG_PYTHON_CMAKE_PATHS}")
    # Last line: the site-packages directory hosting PySide6. Kept separate so
    # the python CTest tests can run the selected interpreter without relying
    # on ambient PYTHONPATH (E1/E2: controlled runtime, no shell assumptions).
    list(LENGTH WELLLOG_PYTHON_CMAKE_PATHS _welllog_py_paths_len)
    if(_welllog_py_paths_len LESS 4)
        message(
            FATAL_ERROR
            "Python profile step returned incomplete PySide6 locations"
        )
    endif()
    math(EXPR _welllog_py_last "${_welllog_py_paths_len} - 1")
    list(GET WELLLOG_PYTHON_CMAKE_PATHS ${_welllog_py_last} WELLLOG_PYTHON_SITE_PACKAGES)
    list(REMOVE_AT WELLLOG_PYTHON_CMAKE_PATHS ${_welllog_py_last})
    list(PREPEND CMAKE_PREFIX_PATH ${WELLLOG_PYTHON_CMAKE_PATHS})
    find_package(Shiboken6 REQUIRED CONFIG)
    find_package(PySide6 REQUIRED CONFIG)
    find_package(
        Shiboken6Tools
        ${PySide6_VERSION}
        EXACT
        REQUIRED
        CONFIG
    )
    get_target_property(
        WELLLOG_SHIBOKEN_GENERATOR_RELEASE
        Shiboken6::shiboken6
        IMPORTED_LOCATION_RELEASE
    )
    if(
        CMAKE_BUILD_TYPE STREQUAL "Debug"
        AND WELLLOG_SHIBOKEN_GENERATOR_RELEASE
    )
        set_target_properties(
            Shiboken6::shiboken6
            PROPERTIES
                IMPORTED_LOCATION_DEBUG
                    "${WELLLOG_SHIBOKEN_GENERATOR_RELEASE}"
        )
    endif()
    if(NOT PySide6_VERSION VERSION_EQUAL Shiboken6_VERSION)
        message(
            FATAL_ERROR
            "PySide6 ${PySide6_VERSION} and Shiboken6 ${Shiboken6_VERSION} must match"
        )
    endif()
    if(NOT PySide6_VERSION VERSION_EQUAL Shiboken6Tools_VERSION)
        message(
            FATAL_ERROR
            "PySide6 ${PySide6_VERSION} and Shiboken6Tools ${Shiboken6Tools_VERSION} must match"
        )
    endif()
    if(NOT Qt6_VERSION VERSION_EQUAL PySide6_VERSION)
        message(
            FATAL_ERROR
            "Qt ${Qt6_VERSION} and PySide6 ${PySide6_VERSION} must use the same SDK profile"
        )
    endif()
    execute_process(
        COMMAND "${CMAKE_CXX_COMPILER}" -print-file-name=include
        OUTPUT_VARIABLE WELLLOG_COMPILER_BUILTIN_INCLUDE
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    set(
        WELLLOG_SHIBOKEN_COMPILER_OPTIONS
        "--compiler-path=${CMAKE_CXX_COMPILER}"
    )
    if(IS_DIRECTORY "${WELLLOG_COMPILER_BUILTIN_INCLUDE}")
        list(
            APPEND
            WELLLOG_SHIBOKEN_COMPILER_OPTIONS
            "-isystem${WELLLOG_COMPILER_BUILTIN_INCLUDE}"
        )
    endif()
endif()

if(WELLLOG_BUILD_SHARED)
    set(WELLLOG_LIBRARY_TYPE SHARED)
else()
    set(WELLLOG_LIBRARY_TYPE STATIC)
endif()

function(welllog_enable_warnings target)
    if(MSVC)
        # C4251/C4275: std::shared_ptr / unique_ptr members in exported PIMPL
        # types — expected noise for MSVC shared builds; not fixable without
        # exporting the entire STL (see #234 Windows shared=ON).
        target_compile_options(
            "${target}"
            PRIVATE /W4 /permissive- /wd4251 /wd4275
        )
        if(WELLLOG_WARNINGS_AS_ERRORS)
            target_compile_options("${target}" PRIVATE /WX)
        endif()
    else()
        target_compile_options(
            "${target}"
            PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
        )
        if(WELLLOG_WARNINGS_AS_ERRORS)
            target_compile_options("${target}" PRIVATE -Werror)
        endif()
    endif()
endfunction()

add_library(welllog_core ${WELLLOG_LIBRARY_TYPE}
    src/core/document.cpp
    src/core/entity_id.cpp
    src/core/utf8.cpp
)
add_library(WellLog::Core ALIAS welllog_core)
set_target_properties(welllog_core PROPERTIES
    EXPORT_NAME Core
    OUTPUT_NAME welllog-core
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION_MAJOR}"
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
)
target_compile_features(welllog_core PUBLIC cxx_std_20)
welllog_enable_warnings(welllog_core)
target_include_directories(welllog_core
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
if(WELLLOG_BUILD_SHARED)
    target_compile_definitions(
        welllog_core
        PRIVATE WELLLOG_CORE_BUILD
        PUBLIC WELLLOG_CORE_SHARED
    )
endif()

# Table Projection (ADR 0022, #154): a Qt-agnostic, virtualized projection of a
# WellLogDocument into per-Sampling-Axis curve tables. Depends only on Core
# (reads raw Curve buffers straight from the document). Qt-facing virtualization
# lives in the qtwidgets adapter.
add_library(welllog_table ${WELLLOG_LIBRARY_TYPE}
    src/table/table_projection.cpp
)
add_library(WellLog::Table ALIAS welllog_table)
set_target_properties(welllog_table PROPERTIES
    EXPORT_NAME Table
    OUTPUT_NAME welllog-table
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION_MAJOR}"
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
)
target_compile_features(welllog_table PUBLIC cxx_std_20)
welllog_enable_warnings(welllog_table)
target_link_libraries(welllog_table PUBLIC WellLog::Core)
target_include_directories(welllog_table
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
if(WELLLOG_BUILD_SHARED)
    target_compile_definitions(
        welllog_table
        PRIVATE WELLLOG_TABLE_BUILD
        PUBLIC WELLLOG_TABLE_SHARED
    )
endif()

add_library(welllog_scene ${WELLLOG_LIBRARY_TYPE}
    src/scene/axis_ticks.cpp
    src/scene/curve_lod.cpp
    src/scene/image_pyramid.cpp
    src/scene/scene.cpp
    src/scene/time_depth.cpp
    src/scene/triangulate.cpp
    src/scene/tst.cpp
)
add_library(WellLog::Scene ALIAS welllog_scene)
set_target_properties(welllog_scene PROPERTIES
    EXPORT_NAME Scene
    OUTPUT_NAME welllog-scene
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION_MAJOR}"
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
)
target_compile_features(welllog_scene PUBLIC cxx_std_20)
welllog_enable_warnings(welllog_scene)
target_link_libraries(welllog_scene PUBLIC WellLog::Core)
target_include_directories(welllog_scene
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
if(WELLLOG_BUILD_SHARED)
    target_compile_definitions(
        welllog_scene
        PRIVATE WELLLOG_SCENE_BUILD
        PUBLIC WELLLOG_SCENE_SHARED
    )
endif()

add_library(welllog_export_vector ${WELLLOG_LIBRARY_TYPE}
    src/export_vector/svg.cpp
    src/export_vector/pagination.cpp
)
add_library(WellLog::ExportVector ALIAS welllog_export_vector)
set_target_properties(welllog_export_vector PROPERTIES
    EXPORT_NAME ExportVector
    OUTPUT_NAME welllog-export-vector
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION_MAJOR}"
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
)
target_compile_features(welllog_export_vector PUBLIC cxx_std_20)
welllog_enable_warnings(welllog_export_vector)
target_link_libraries(welllog_export_vector PUBLIC WellLog::Scene)
target_include_directories(welllog_export_vector
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
if(WELLLOG_BUILD_SHARED)
    target_compile_definitions(
        welllog_export_vector
        PRIVATE WELLLOG_EXPORT_VECTOR_BUILD
        PUBLIC WELLLOG_EXPORT_VECTOR_SHARED
    )
endif()

# Table export backends (ADR 0022 / table-and-export.md §5-§7, §10, #155): CSV,
# versioned XML, and XLSX writers over a TableProjection. Depends only on Table
# (which pulls Core) — Qt stays in the adapter. XLSX adds PRIVATE ZLIB for the
# self-contained OOXML zip writer.
add_library(welllog_export_table ${WELLLOG_LIBRARY_TYPE}
    src/export_table/csv.cpp
    src/export_table/xml.cpp
    src/export_table/xlsx.cpp
    src/export_table/zip_writer.cpp
)
add_library(WellLog::ExportTable ALIAS welllog_export_table)
set_target_properties(welllog_export_table PROPERTIES
    EXPORT_NAME ExportTable
    OUTPUT_NAME welllog-export-table
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION_MAJOR}"
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
)
target_compile_features(welllog_export_table PUBLIC cxx_std_20)
welllog_enable_warnings(welllog_export_table)
target_link_libraries(welllog_export_table
    PUBLIC WellLog::Table
    PRIVATE ZLIB::ZLIB
)
target_include_directories(welllog_export_table
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
if(WELLLOG_BUILD_SHARED)
    target_compile_definitions(
        welllog_export_table
        PRIVATE WELLLOG_EXPORT_TABLE_BUILD
        PUBLIC WELLLOG_EXPORT_TABLE_SHARED
    )
endif()

# PDF export backend (ADR: hand-rolled writer, #185; scene emission #187). No
# third-party PDF library; only zlib for FlateDecode stream compression.
add_library(welllog_export_pdf ${WELLLOG_LIBRARY_TYPE}
    src/export_vector/pdf.cpp
    src/export_vector/pdf_scene.cpp
)
add_library(WellLog::ExportPdf ALIAS welllog_export_pdf)
set_target_properties(welllog_export_pdf PROPERTIES
    EXPORT_NAME ExportPdf
    OUTPUT_NAME welllog-export-pdf
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION_MAJOR}"
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
)
target_compile_features(welllog_export_pdf PUBLIC cxx_std_20)
welllog_enable_warnings(welllog_export_pdf)
target_link_libraries(welllog_export_pdf
    PUBLIC WellLog::Scene
    PRIVATE ZLIB::ZLIB
)
target_include_directories(welllog_export_pdf
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
if(WELLLOG_BUILD_SHARED)
    target_compile_definitions(
        welllog_export_pdf
        PRIVATE WELLLOG_EXPORT_PDF_BUILD
        PUBLIC WELLLOG_EXPORT_PDF_SHARED
    )
endif()

# CGM Version 3 Binary subset (B1.CGM.1 / ADR 0054). Self-written; no third-party
# CGM SDK. Skeleton: metafile + one PICTURE, curves + track frames + Latin text.
add_library(welllog_export_cgm ${WELLLOG_LIBRARY_TYPE}
    src/export_vector/cgm.cpp
)
add_library(WellLog::ExportCgm ALIAS welllog_export_cgm)
set_target_properties(welllog_export_cgm PROPERTIES
    EXPORT_NAME ExportCgm
    OUTPUT_NAME welllog-export-cgm
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION_MAJOR}"
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
)
target_compile_features(welllog_export_cgm PUBLIC cxx_std_20)
welllog_enable_warnings(welllog_export_cgm)
target_link_libraries(welllog_export_cgm PUBLIC WellLog::Scene)
target_include_directories(welllog_export_cgm
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
if(WELLLOG_BUILD_SHARED)
    target_compile_definitions(
        welllog_export_cgm
        PRIVATE WELLLOG_EXPORT_CGM_BUILD
        PUBLIC WELLLOG_EXPORT_CGM_SHARED
    )
endif()

# Raster PNG/TIFF export (#157, ADR 0021). CPU tiled rendering + zlib PNG /
# hand-rolled TIFF; never QOpenGLWidget framebuffer readback.
add_library(welllog_export_raster ${WELLLOG_LIBRARY_TYPE}
    src/export_raster/raster_export.cpp
)
add_library(WellLog::ExportRaster ALIAS welllog_export_raster)
set_target_properties(welllog_export_raster PROPERTIES
    EXPORT_NAME ExportRaster
    OUTPUT_NAME welllog-export-raster
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION_MAJOR}"
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
)
target_compile_features(welllog_export_raster PUBLIC cxx_std_20)
welllog_enable_warnings(welllog_export_raster)
target_link_libraries(welllog_export_raster
    PUBLIC WellLog::ExportVector WellLog::Scene
    PRIVATE ZLIB::ZLIB Threads::Threads
)
target_include_directories(welllog_export_raster
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
if(WELLLOG_BUILD_SHARED)
    target_compile_definitions(
        welllog_export_raster
        PRIVATE WELLLOG_EXPORT_RASTER_BUILD
        PUBLIC WELLLOG_EXPORT_RASTER_SHARED
    )
endif()

add_library(welllog_session ${WELLLOG_LIBRARY_TYPE}
    src/session/session.cpp
    src/session/observability.cpp
)
add_library(WellLog::Session ALIAS welllog_session)
set_target_properties(welllog_session PROPERTIES
    EXPORT_NAME Session
    OUTPUT_NAME welllog-session
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION_MAJOR}"
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
)
target_compile_features(welllog_session PUBLIC cxx_std_20)
welllog_enable_warnings(welllog_session)
target_link_libraries(
    welllog_session
    PUBLIC WellLog::Core WellLog::Scene
    PRIVATE Threads::Threads
)
target_include_directories(welllog_session
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
if(WELLLOG_BUILD_SHARED)
    target_compile_definitions(
        welllog_session
        PRIVATE WELLLOG_SESSION_BUILD
        PUBLIC WELLLOG_SESSION_SHARED
    )
endif()

if(WELLLOG_BUILD_ARROW)
    add_library(welllog_arrow ${WELLLOG_LIBRARY_TYPE}
        src/arrow/adapter.cpp
    )
    add_library(WellLog::Arrow ALIAS welllog_arrow)
    set_target_properties(welllog_arrow PROPERTIES
        EXPORT_NAME Arrow
        OUTPUT_NAME welllog-arrow
        VERSION "${PROJECT_VERSION}"
        SOVERSION "${PROJECT_VERSION_MAJOR}"
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN YES
    )
    target_compile_features(welllog_arrow PUBLIC cxx_std_20)
    welllog_enable_warnings(welllog_arrow)
    # PRIVATE IO for container_security / mmap limits (#171).
    target_link_libraries(welllog_arrow PUBLIC WellLog::Core PRIVATE WellLog::IO)
    target_include_directories(welllog_arrow
        PUBLIC
            "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
            "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
    )
    if(WELLLOG_ARROW_HAS_IPC)
        target_compile_definitions(welllog_arrow PUBLIC WELLLOG_ARROW_HAS_IPC=1)
        target_link_libraries(welllog_arrow PRIVATE Arrow::arrow_shared)
    endif()
    if(WELLLOG_BUILD_SHARED)
        target_compile_definitions(
            welllog_arrow
            PRIVATE WELLLOG_ARROW_BUILD
            PUBLIC WELLLOG_ARROW_SHARED
        )
    endif()
endif()

add_library(welllog_io ${WELLLOG_LIBRARY_TYPE}
    src/io/asset_security.cpp
    src/io/container_security.cpp
    src/io/dlis.cpp
    src/io/format716.cpp
    src/io/las.cpp
    src/io/lis.cpp
    src/io/manifest.cpp
)
add_library(WellLog::IO ALIAS welllog_io)
set_target_properties(welllog_io PROPERTIES
    EXPORT_NAME IO
    OUTPUT_NAME welllog-io
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION_MAJOR}"
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
)
target_compile_features(welllog_io PUBLIC cxx_std_20)
welllog_enable_warnings(welllog_io)
target_link_libraries(welllog_io PUBLIC WellLog::Core PRIVATE ZLIB::ZLIB)
target_include_directories(welllog_io
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
if(WELLLOG_BUILD_SHARED)
    target_compile_definitions(
        welllog_io
        PRIVATE WELLLOG_IO_BUILD
        PUBLIC WELLLOG_IO_SHARED
    )
endif()

add_library(welllog_render_gl ${WELLLOG_LIBRARY_TYPE}
    src/render_gl/capability.cpp
    src/render_gl/raster.cpp
    src/render_gl/renderer.cpp
    src/render_gl/upload.cpp
)
add_library(WellLog::RenderGL ALIAS welllog_render_gl)
set_target_properties(welllog_render_gl PROPERTIES
    EXPORT_NAME RenderGL
    OUTPUT_NAME welllog-render-gl
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION_MAJOR}"
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
)
target_compile_features(welllog_render_gl PUBLIC cxx_std_20)
welllog_enable_warnings(welllog_render_gl)
target_link_libraries(welllog_render_gl PUBLIC WellLog::Scene)
target_include_directories(welllog_render_gl
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
if(WELLLOG_BUILD_SHARED)
    target_compile_definitions(
        welllog_render_gl
        PRIVATE WELLLOG_RENDER_GL_BUILD
        PUBLIC WELLLOG_RENDER_GL_SHARED
    )
endif()

if(WELLLOG_BUILD_TEXT)
    add_library(welllog_text ${WELLLOG_LIBRARY_TYPE}
        src/text/harfbuzz_text_engine.cpp
    )
    add_library(WellLog::Text ALIAS welllog_text)
    set_target_properties(welllog_text PROPERTIES
        EXPORT_NAME Text
        OUTPUT_NAME welllog-text
        VERSION "${PROJECT_VERSION}"
        SOVERSION "${PROJECT_VERSION_MAJOR}"
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN YES
    )
    target_compile_features(welllog_text PUBLIC cxx_std_20)
    welllog_enable_warnings(welllog_text)
    target_link_libraries(
        welllog_text
        PUBLIC WellLog::Scene
        PRIVATE
            PkgConfig::HarfBuzz
            PkgConfig::FreeType
            PkgConfig::ICU_UC
            PkgConfig::ICU_I18N
    )
    target_include_directories(welllog_text
        PUBLIC
            "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
            "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    if(WELLLOG_BUILD_SHARED)
        target_compile_definitions(
            welllog_text
            PRIVATE WELLLOG_TEXT_BUILD
            PUBLIC WELLLOG_TEXT_SHARED
        )
    endif()
endif()

if(WELLLOG_BUILD_QT_WIDGETS)
    qt_add_library(welllog_qtwidgets ${WELLLOG_LIBRARY_TYPE}
        src/qtwidgets/well_log_view.cpp
        src/qtwidgets/table_model.cpp
        include/welllog/qtwidgets/well_log_view.hpp
        include/welllog/qtwidgets/table_model.hpp
    )
    add_library(WellLog::QtWidgets ALIAS welllog_qtwidgets)
    set_target_properties(welllog_qtwidgets PROPERTIES
        EXPORT_NAME QtWidgets
        OUTPUT_NAME welllog-qtwidgets
        VERSION "${PROJECT_VERSION}"
        SOVERSION "${PROJECT_VERSION_MAJOR}"
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN YES
    )
    target_compile_features(welllog_qtwidgets PUBLIC cxx_std_20)
    welllog_enable_warnings(welllog_qtwidgets)
    target_link_libraries(
        welllog_qtwidgets
        PUBLIC
            WellLog::ExportPdf
            WellLog::ExportCgm
            WellLog::ExportVector
            WellLog::RenderGL
            WellLog::Session
            WellLog::Table
            Qt6::OpenGLWidgets
            Qt6::Widgets
    )
    if(WELLLOG_BUILD_TEXT)
        target_link_libraries(welllog_qtwidgets PUBLIC WellLog::Text)
        target_compile_definitions(
            welllog_qtwidgets
            PRIVATE WELLLOG_WITH_TEXT
        )
    endif()
    target_include_directories(welllog_qtwidgets
        PUBLIC
            "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
            "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    if(WELLLOG_BUILD_SHARED)
        target_compile_definitions(
            welllog_qtwidgets
            PRIVATE WELLLOG_QTWIDGETS_BUILD
            PUBLIC WELLLOG_QTWIDGETS_SHARED
        )
    endif()
endif()

if(WELLLOG_BUILD_PYTHON)
    set(WELLLOG_PYTHON_GENERATED_SOURCES
        "${CMAKE_CURRENT_BINARY_DIR}/welllog/_QtWidgets/_qtwidgets_module_wrapper.cpp"
        "${CMAKE_CURRENT_BINARY_DIR}/welllog/_QtWidgets/welllog_wrapper.cpp"
        "${CMAKE_CURRENT_BINARY_DIR}/welllog/_QtWidgets/welllog_welllogview_wrapper.cpp"
    )
    set(FORCE_LIMITED_API OFF)
    shiboken_generator_create_binding(
        EXTENSION_TARGET _QtWidgets
        GENERATED_SOURCES ${WELLLOG_PYTHON_GENERATED_SOURCES}
        HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/src/python/bindings.hpp"
        TYPESYSTEM_FILE
            "${CMAKE_CURRENT_SOURCE_DIR}/src/python/typesystem_welllog.xml"
        LIBRARY_TARGET welllog_qtwidgets
        QT_MODULES Core Gui Widgets OpenGLWidgets
        SHIBOKEN_EXTRA_OPTIONS
            "-I${CMAKE_CURRENT_SOURCE_DIR}/include"
            "-I${CMAKE_CURRENT_SOURCE_DIR}/src/python"
            "--language-level=c++20"
            ${WELLLOG_SHIBOKEN_COMPILER_OPTIONS}
    )
    target_sources(
        _QtWidgets
        PRIVATE
            src/python/numpy_bridge.cpp
            src/python/numpy_bridge.hpp
    )
    target_include_directories(
        _QtWidgets
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/include"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/python"
    )
    set_target_properties(
        _QtWidgets
        PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY
                "${CMAKE_CURRENT_BINARY_DIR}/python/welllog"
            RUNTIME_OUTPUT_DIRECTORY
                "${CMAKE_CURRENT_BINARY_DIR}/python/welllog"
    )
    file(
        COPY
            "${CMAKE_CURRENT_SOURCE_DIR}/python/welllog/__init__.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/python/welllog/errors.py"
        DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/python/welllog"
    )
endif()

if(WELLLOG_BUILD_BENCHMARKS)
    add_executable(welllog_dense_curve_benchmark
        benchmarks/dense_curve_benchmark.cpp
    )
    target_compile_features(
        welllog_dense_curve_benchmark
        PRIVATE cxx_std_20
    )
    welllog_enable_warnings(welllog_dense_curve_benchmark)
    target_link_libraries(
        welllog_dense_curve_benchmark
        PRIVATE WellLog::RenderGL WellLog::Session Qt6::Gui
    )
    target_include_directories(
        welllog_dense_curve_benchmark
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
endif()

install(
    TARGETS
        welllog_core
        welllog_table
        welllog_scene
        welllog_export_vector
        welllog_export_pdf
        welllog_export_cgm
        welllog_export_raster
        welllog_export_table
        welllog_session
        welllog_io
        welllog_render_gl
    EXPORT WellLogTargets
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)
if(WELLLOG_BUILD_ARROW)
    install(
        TARGETS welllog_arrow
        EXPORT WellLogTargets
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    )
endif()
if(WELLLOG_BUILD_TEXT)
    install(
        TARGETS welllog_text
        EXPORT WellLogTargets
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    )
endif()
if(WELLLOG_BUILD_QT_WIDGETS)
    install(
        TARGETS welllog_qtwidgets
        EXPORT WellLogTargets
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    )
endif()
if(WELLLOG_BUILD_PYTHON)
    install(
        TARGETS _QtWidgets
        LIBRARY DESTINATION welllog COMPONENT Python
        RUNTIME DESTINATION welllog COMPONENT Python
    )
    install(
        FILES
            python/welllog/__init__.py
            python/welllog/__init__.pyi
            python/welllog/errors.py
            python/welllog/errors.pyi
            python/welllog/py.typed
        DESTINATION welllog
        COMPONENT Python
    )
endif()
install(
    DIRECTORY include/
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    FILES_MATCHING PATTERN "*.hpp"
)
install(
    DIRECTORY schemas/
    DESTINATION "${CMAKE_INSTALL_DATADIR}/welllog/schemas"
    FILES_MATCHING PATTERN "*.json"
)
if(WELLLOG_FETCHED_ZLIB AND DEFINED WELLLOG_ZLIB_INSTALL_TARGET)
    # Ship the bundled static zlib next to WellLog so package.consumer can
    # resolve ZLIB::ZLIB without a system install (Windows CI path).
    install(
        TARGETS ${WELLLOG_ZLIB_INSTALL_TARGET}
        EXPORT WellLogZlibTargets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
        INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    )
    if(DEFINED welllog_zlib_SOURCE_DIR)
        install(
            FILES
                "${welllog_zlib_SOURCE_DIR}/zlib.h"
                "${welllog_zlib_BINARY_DIR}/zconf.h"
            DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
        )
    endif()
    install(
        EXPORT WellLogZlibTargets
        FILE WellLogZlibTargets.cmake
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/WellLog"
    )
endif()
install(
    EXPORT WellLogTargets
    FILE WellLogTargets.cmake
    NAMESPACE WellLog::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/WellLog"
)
configure_package_config_file(
    cmake/WellLogConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/WellLogConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/WellLog"
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/WellLogConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMajorVersion
)
install(
    FILES
        "${CMAKE_CURRENT_BINARY_DIR}/WellLogConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/WellLogConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/WellLog"
)

if(WELLLOG_BUILD_TESTS)
    add_executable(welllog_curve_lod_tests
        tests/scene/curve_lod_test.cpp
    )
    target_compile_features(welllog_curve_lod_tests PRIVATE cxx_std_20)
    welllog_enable_warnings(welllog_curve_lod_tests)
    target_link_libraries(welllog_curve_lod_tests PRIVATE WellLog::Scene)
    add_test(NAME welllog.curve-lod COMMAND welllog_curve_lod_tests)

    add_executable(welllog_dense_curve_pipeline_tests
        tests/integration/dense_curve_pipeline_test.cpp
    )
    target_compile_features(
        welllog_dense_curve_pipeline_tests
        PRIVATE cxx_std_20
    )
    welllog_enable_warnings(welllog_dense_curve_pipeline_tests)
    target_link_libraries(
        welllog_dense_curve_pipeline_tests
        PRIVATE WellLog::RenderGL WellLog::Session
    )
    add_test(
        NAME welllog.dense-curve-pipeline
        COMMAND welllog_dense_curve_pipeline_tests
    )

    # #173 — headless LRW / cancel / session-destroy / table+SVG without GL.
    add_executable(welllog_async_lrw_stress_tests
        tests/integration/async_lrw_stress_test.cpp
    )
    target_compile_features(welllog_async_lrw_stress_tests PRIVATE cxx_std_20)
    welllog_enable_warnings(welllog_async_lrw_stress_tests)
    target_link_libraries(
        welllog_async_lrw_stress_tests
        PRIVATE
            WellLog::ExportRaster
            WellLog::ExportVector
            WellLog::Session
            WellLog::Table
    )
    add_test(
        NAME welllog.async-lrw-stress
        COMMAND welllog_async_lrw_stress_tests
    )

    # #174 — ADR 0014 reference scenario (CI scale by default; full 1e8 via env).
    add_executable(welllog_release_gate_scenario_tests
        tests/integration/release_gate_scenario_test.cpp
    )
    target_compile_features(
        welllog_release_gate_scenario_tests
        PRIVATE cxx_std_20
    )
    welllog_enable_warnings(welllog_release_gate_scenario_tests)
    target_link_libraries(
        welllog_release_gate_scenario_tests
        PRIVATE WellLog::ExportVector WellLog::RenderGL WellLog::Session
    )
    add_test(
        NAME welllog.release-gate-scenario
        COMMAND welllog_release_gate_scenario_tests
    )
    # Full 1e8 is opt-in (fixed workstation); CI only runs the reduced scale.
    add_test(
        NAME welllog.release-gate-scenario-full
        COMMAND welllog_release_gate_scenario_tests
    )
    set_tests_properties(
        welllog.release-gate-scenario-full
        PROPERTIES
            ENVIRONMENT "WELLLOG_GATE_SCALE=full"
            LABELS "release-gate-full"
            TIMEOUT 1800
    )

    add_executable(welllog_session_tests
        tests/integration/session_submission_test.cpp
    )
    target_compile_features(welllog_session_tests PRIVATE cxx_std_20)
    welllog_enable_warnings(welllog_session_tests)
    target_link_libraries(welllog_session_tests PRIVATE WellLog::Session)
    add_test(NAME welllog.session COMMAND welllog_session_tests)

    add_executable(welllog_document_annotation_tests
        tests/integration/document_annotation_test.cpp
    )
    target_compile_features(
        welllog_document_annotation_tests
        PRIVATE cxx_std_20
    )
    welllog_enable_warnings(welllog_document_annotation_tests)
    target_link_libraries(
        welllog_document_annotation_tests
        PRIVATE WellLog::Session
    )
    add_test(
        NAME welllog.document-annotations
        COMMAND welllog_document_annotation_tests
    )

    add_executable(welllog_multi_rate_curve_tests
        tests/integration/multi_rate_curve_test.cpp
    )
    target_compile_features(
        welllog_multi_rate_curve_tests
        PRIVATE cxx_std_20
    )
    welllog_enable_warnings(welllog_multi_rate_curve_tests)
    target_link_libraries(
        welllog_multi_rate_curve_tests
        PRIVATE WellLog::Session WellLog::Table WellLog::Scene WellLog::IO
    )
    add_test(
        NAME welllog.multi-rate-curves
        COMMAND welllog_multi_rate_curve_tests
    )

    add_executable(welllog_layer_preparation_tests
        tests/integration/layer_preparation_test.cpp
    )
    target_compile_features(
        welllog_layer_preparation_tests
        PRIVATE cxx_std_20
    )
    welllog_enable_warnings(welllog_layer_preparation_tests)
    target_link_libraries(
        welllog_layer_preparation_tests
        PRIVATE WellLog::Session
    )
    add_test(
        NAME welllog.layer-preparation
        COMMAND welllog_layer_preparation_tests
    )

    add_executable(welllog_time_depth_tests
        tests/integration/time_depth_test.cpp
    )
    target_compile_features(
        welllog_time_depth_tests
        PRIVATE cxx_std_20
    )
    welllog_enable_warnings(welllog_time_depth_tests)
    target_link_libraries(
        welllog_time_depth_tests
        PRIVATE WellLog::Scene
    )
    add_test(
        NAME welllog.time-depth
        COMMAND welllog_time_depth_tests
    )

    add_executable(welllog_axis_ticks_tests
        tests/integration/axis_ticks_test.cpp
    )
    target_compile_features(
        welllog_axis_ticks_tests
        PRIVATE cxx_std_20
    )
    welllog_enable_warnings(welllog_axis_ticks_tests)
    target_link_libraries(
        welllog_axis_ticks_tests
        PRIVATE WellLog::Scene
    )
    add_test(
        NAME welllog.axis-ticks
        COMMAND welllog_axis_ticks_tests
    )

    add_executable(welllog_tst_tests
        tests/integration/tst_test.cpp
    )
    target_compile_features(
        welllog_tst_tests
        PRIVATE cxx_std_20
    )
    welllog_enable_warnings(welllog_tst_tests)
    target_link_libraries(
        welllog_tst_tests
        PRIVATE WellLog::Scene
    )
    add_test(
        NAME welllog.tst
        COMMAND welllog_tst_tests
    )

    add_executable(welllog_tst_layers_tests
        tests/integration/tst_layers_test.cpp
    )
    target_compile_features(
        welllog_tst_layers_tests
        PRIVATE cxx_std_20
    )
    welllog_enable_warnings(welllog_tst_layers_tests)
    target_link_libraries(
        welllog_tst_layers_tests
        PRIVATE WellLog::Scene
    )
    add_test(
        NAME welllog.tst-layers
        COMMAND welllog_tst_layers_tests
    )

    if(WELLLOG_BUILD_TEXT)
        add_executable(welllog_text_engine_tests
            tests/text/text_engine_test.cpp
        )
        target_compile_features(
            welllog_text_engine_tests
            PRIVATE cxx_std_20
        )
        welllog_enable_warnings(welllog_text_engine_tests)
        target_compile_definitions(
            welllog_text_engine_tests
            PRIVATE
                "WELLLOG_TEST_FONT_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/assets/fonts\""
        )
        target_link_libraries(
            welllog_text_engine_tests
            PRIVATE WellLog::Text
        )
        add_test(
            NAME welllog.text-engine
            COMMAND welllog_text_engine_tests
        )
    endif()

    if(WELLLOG_BUILD_TEXT)
        add_executable(welllog_text_scene_tests
            tests/integration/text_scene_test.cpp
        )
        target_compile_features(
            welllog_text_scene_tests
            PRIVATE cxx_std_20
        )
        welllog_enable_warnings(welllog_text_scene_tests)
        target_compile_definitions(
            welllog_text_scene_tests
            PRIVATE
                "WELLLOG_TEST_FONT_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/assets/fonts\""
        )
        target_link_libraries(
            welllog_text_scene_tests
            PRIVATE WellLog::Text WellLog::Session
        )
        add_test(
            NAME welllog.text-scene
            COMMAND welllog_text_scene_tests
        )

        add_executable(welllog_layer_svg_tests
            tests/integration/layer_svg_test.cpp
        )
        target_compile_features(
            welllog_layer_svg_tests
            PRIVATE cxx_std_20
        )
        welllog_enable_warnings(welllog_layer_svg_tests)
        target_compile_definitions(
            welllog_layer_svg_tests
            PRIVATE
                "WELLLOG_TEST_FONT_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/assets/fonts\""
        )
        target_link_libraries(
            welllog_layer_svg_tests
            PRIVATE WellLog::ExportVector WellLog::Text WellLog::Session
        )
        add_test(
            NAME welllog.layer-svg
            COMMAND welllog_layer_svg_tests
        )

        add_executable(welllog_scene_snapshot_tests
            tests/integration/scene_snapshot_test.cpp
        )
        target_compile_features(
            welllog_scene_snapshot_tests
            PRIVATE cxx_std_20
        )
        welllog_enable_warnings(welllog_scene_snapshot_tests)
        target_compile_definitions(
            welllog_scene_snapshot_tests
            PRIVATE
                "WELLLOG_TEST_FONT_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/assets/fonts\""
        )
        target_link_libraries(
            welllog_scene_snapshot_tests
            PRIVATE WellLog::Text WellLog::Session
        )
        add_test(
            NAME welllog.scene-snapshot
            COMMAND welllog_scene_snapshot_tests
        )
    endif()

    # Multi-scale track headers exercise HarfBuzz text; skip when TEXT=OFF (#234).
    if(WELLLOG_BUILD_TEXT)
        add_executable(welllog_multi_scale_tests
            tests/integration/multi_scale_track_test.cpp
        )
        target_compile_features(welllog_multi_scale_tests PRIVATE cxx_std_20)
        welllog_enable_warnings(welllog_multi_scale_tests)
        target_compile_definitions(
            welllog_multi_scale_tests
            PRIVATE
                "WELLLOG_TEST_FONT_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/assets/fonts\""
        )
        target_link_libraries(
            welllog_multi_scale_tests
            PRIVATE WellLog::ExportVector WellLog::RenderGL WellLog::Text WellLog::Session
        )
        add_test(
            NAME welllog.multi-scale-track
            COMMAND welllog_multi_scale_tests
        )
    endif()

    add_executable(welllog_crossover_fill_tests
        tests/integration/crossover_fill_test.cpp
    )
    target_compile_features(welllog_crossover_fill_tests PRIVATE cxx_std_20)
    welllog_enable_warnings(welllog_crossover_fill_tests)
    target_link_libraries(
        welllog_crossover_fill_tests
        PRIVATE WellLog::ExportVector WellLog::RenderGL WellLog::Session
    )
    add_test(
        NAME welllog.crossover-fill
        COMMAND welllog_crossover_fill_tests
    )

    add_executable(welllog_image_layer_tests
        tests/integration/image_layer_test.cpp
    )
    target_compile_features(welllog_image_layer_tests PRIVATE cxx_std_20)
    welllog_enable_warnings(welllog_image_layer_tests)
    target_include_directories(
        welllog_image_layer_tests
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        welllog_image_layer_tests
        PRIVATE WellLog::ExportVector WellLog::Session
    )
    add_test(
        NAME welllog.image-layer
        COMMAND welllog_image_layer_tests
    )

    add_executable(welllog_custom_layer_tests
        tests/integration/custom_layer_test.cpp
    )
    target_compile_features(welllog_custom_layer_tests PRIVATE cxx_std_20)
    welllog_enable_warnings(welllog_custom_layer_tests)
    target_include_directories(
        welllog_custom_layer_tests
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        welllog_custom_layer_tests
        PRIVATE WellLog::ExportVector WellLog::RenderGL WellLog::Session
    )
    add_test(
        NAME welllog.custom-layer
        COMMAND welllog_custom_layer_tests
    )

    add_executable(welllog_manifest_tests
        tests/integration/manifest_roundtrip_test.cpp
    )
    target_compile_features(welllog_manifest_tests PRIVATE cxx_std_20)
    welllog_enable_warnings(welllog_manifest_tests)
    target_link_libraries(
        welllog_manifest_tests
        PRIVATE WellLog::IO WellLog::Session
    )
    add_test(NAME welllog.manifest COMMAND welllog_manifest_tests)

    add_executable(welllog_las_adapter_tests
        tests/integration/las_adapter_test.cpp
    )
    target_compile_features(welllog_las_adapter_tests PRIVATE cxx_std_20)
    welllog_enable_warnings(welllog_las_adapter_tests)
    target_link_libraries(
        welllog_las_adapter_tests
        PRIVATE WellLog::ExportVector WellLog::IO WellLog::Session WellLog::Table
    )
    add_test(NAME welllog.las-adapter COMMAND welllog_las_adapter_tests)

    add_executable(welllog_dlis_adapter_tests
        tests/integration/dlis_adapter_test.cpp
    )
    target_compile_features(welllog_dlis_adapter_tests PRIVATE cxx_std_20)
    welllog_enable_warnings(welllog_dlis_adapter_tests)
    target_link_libraries(
        welllog_dlis_adapter_tests
        PRIVATE WellLog::ExportVector WellLog::IO WellLog::Session WellLog::Table
    )
    add_test(NAME welllog.dlis-adapter COMMAND welllog_dlis_adapter_tests)

    add_executable(welllog_lis_adapter_tests
        tests/integration/lis_adapter_test.cpp
    )
    target_compile_features(welllog_lis_adapter_tests PRIVATE cxx_std_20)
    welllog_enable_warnings(welllog_lis_adapter_tests)
    target_link_libraries(
        welllog_lis_adapter_tests
        PRIVATE WellLog::ExportVector WellLog::IO WellLog::Session WellLog::Table
    )
    add_test(NAME welllog.lis-adapter COMMAND welllog_lis_adapter_tests)

    add_executable(welllog_format716_adapter_tests
        tests/integration/format716_adapter_test.cpp
    )
    target_compile_features(welllog_format716_adapter_tests PRIVATE cxx_std_20)
    welllog_enable_warnings(welllog_format716_adapter_tests)
    target_link_libraries(
        welllog_format716_adapter_tests
        PRIVATE WellLog::ExportVector WellLog::ExportTable WellLog::IO
                WellLog::Session WellLog::Table
    )
    add_test(NAME welllog.format716-adapter COMMAND welllog_format716_adapter_tests)

    add_executable(welllog_headless_svg_tests
        tests/integration/headless_svg_test.cpp
    )
    target_compile_features(welllog_headless_svg_tests PRIVATE cxx_std_20)
    welllog_enable_warnings(welllog_headless_svg_tests)
    target_link_libraries(
        welllog_headless_svg_tests
        PRIVATE WellLog::ExportVector WellLog::Session
    )
    add_test(NAME welllog.headless-svg COMMAND welllog_headless_svg_tests)

    add_executable(welllog_paginated_svg_tests
        tests/integration/paginated_svg_test.cpp
    )
    target_compile_features(welllog_paginated_svg_tests PRIVATE cxx_std_20)
    welllog_enable_warnings(welllog_paginated_svg_tests)
    target_link_libraries(
        welllog_paginated_svg_tests
        PRIVATE WellLog::ExportVector WellLog::Session
    )
    add_test(NAME welllog.paginated-svg COMMAND welllog_paginated_svg_tests)

    add_executable(welllog_pdf_spike_tests
        tests/integration/pdf_spike_test.cpp
    )
    target_compile_features(welllog_pdf_spike_tests PRIVATE cxx_std_20)
    welllog_enable_warnings(welllog_pdf_spike_tests)
    target_link_libraries(
        welllog_pdf_spike_tests
        PRIVATE WellLog::ExportPdf ZLIB::ZLIB
    )
    add_test(NAME welllog.pdf-spike COMMAND welllog_pdf_spike_tests)

    add_executable(welllog_cgm_spike_tests
        tests/integration/cgm_spike_test.cpp
    )
    target_compile_features(welllog_cgm_spike_tests PRIVATE cxx_std_20)
    welllog_enable_warnings(welllog_cgm_spike_tests)
    target_link_libraries(
        welllog_cgm_spike_tests
        PRIVATE WellLog::ExportCgm WellLog::Session WellLog::Scene
    )
    add_test(NAME welllog.cgm-spike COMMAND welllog_cgm_spike_tests)

    # PDF scene/band text needs HarfBuzz; skip when TEXT=OFF (#234 Windows CI).
    if(WELLLOG_BUILD_TEXT)
        add_executable(welllog_pdf_scene_tests
            tests/integration/pdf_scene_test.cpp
        )
        target_compile_features(welllog_pdf_scene_tests PRIVATE cxx_std_20)
        welllog_enable_warnings(welllog_pdf_scene_tests)
        target_compile_definitions(
            welllog_pdf_scene_tests
            PRIVATE
                "WELLLOG_TEST_FONT_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/assets/fonts\""
        )
        target_link_libraries(
            welllog_pdf_scene_tests
            PRIVATE WellLog::ExportPdf WellLog::Text WellLog::Session ZLIB::ZLIB
        )
        add_test(NAME welllog.pdf-scene COMMAND welllog_pdf_scene_tests)

        add_executable(welllog_pdf_scene_full_tests
            tests/integration/pdf_scene_full_test.cpp
        )
        target_compile_features(welllog_pdf_scene_full_tests PRIVATE cxx_std_20)
        welllog_enable_warnings(welllog_pdf_scene_full_tests)
        target_compile_definitions(
            welllog_pdf_scene_full_tests
            PRIVATE
                "WELLLOG_TEST_FONT_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/assets/fonts\""
        )
        target_include_directories(
            welllog_pdf_scene_full_tests
            PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
        )
        target_link_libraries(
            welllog_pdf_scene_full_tests
            PRIVATE WellLog::ExportPdf WellLog::Scene WellLog::Text WellLog::Session ZLIB::ZLIB
        )
        add_test(NAME welllog.pdf-scene-full COMMAND welllog_pdf_scene_full_tests)
    endif()

    add_executable(welllog_raster_export_tests
        tests/integration/raster_export_test.cpp
    )
    target_compile_features(welllog_raster_export_tests PRIVATE cxx_std_20)
    welllog_enable_warnings(welllog_raster_export_tests)
    target_link_libraries(
        welllog_raster_export_tests
        PRIVATE WellLog::ExportRaster WellLog::Session ZLIB::ZLIB
    )
    add_test(NAME welllog.raster-export COMMAND welllog_raster_export_tests)

    # SDK marker symbols (Epic C 收尾 slice 1): semantic → glyph mapping,
    # shared geometry, and SVG/PDF/raster marker-symbol emission.
    add_executable(welllog_marker_symbol_tests
        tests/integration/marker_symbol_test.cpp
    )
    target_compile_features(welllog_marker_symbol_tests PRIVATE cxx_std_20)
    welllog_enable_warnings(welllog_marker_symbol_tests)
    target_link_libraries(
        welllog_marker_symbol_tests
        PRIVATE
            WellLog::ExportPdf
            WellLog::ExportRaster
            WellLog::ExportVector
            WellLog::Scene
            WellLog::Session
            ZLIB::ZLIB
    )
    add_test(NAME welllog.marker-symbols COMMAND welllog_marker_symbol_tests)

    # TVD/TVDSS domain interval-track projection (Epic C 收尾 slice 2): a
    # decreasing display-depth transform (TVDSS) projects intervals/markers
    # correctly and stays reversible.
    add_executable(welllog_tvdss_projection_tests
        tests/integration/tvdss_projection_test.cpp
    )
    target_compile_features(welllog_tvdss_projection_tests PRIVATE cxx_std_20)
    welllog_enable_warnings(welllog_tvdss_projection_tests)
    target_link_libraries(
        welllog_tvdss_projection_tests
        PRIVATE WellLog::ExportVector WellLog::Scene WellLog::Session
    )
    add_test(NAME welllog.tvdss-projection COMMAND welllog_tvdss_projection_tests)

    add_executable(welllog_qc_derived_curve_tests
        tests/integration/qc_derived_curve_test.cpp
    )
    target_compile_features(welllog_qc_derived_curve_tests PRIVATE cxx_std_20)
    welllog_enable_warnings(welllog_qc_derived_curve_tests)
    target_link_libraries(
        welllog_qc_derived_curve_tests
        PRIVATE WellLog::ExportVector WellLog::Session WellLog::Table
    )
    add_test(NAME welllog.qc-derived-curve COMMAND welllog_qc_derived_curve_tests)

    add_executable(welllog_multi_well_surface_tests
        tests/integration/multi_well_surface_test.cpp
    )
    target_compile_features(welllog_multi_well_surface_tests PRIVATE cxx_std_20)
    welllog_enable_warnings(welllog_multi_well_surface_tests)
    target_link_libraries(
        welllog_multi_well_surface_tests
        PRIVATE WellLog::ExportVector WellLog::Session
    )
    add_test(NAME welllog.multi-well-surface COMMAND welllog_multi_well_surface_tests)

    add_executable(welllog_depth_transform_overlay_tests
        tests/integration/depth_transform_overlay_test.cpp
    )
    target_compile_features(welllog_depth_transform_overlay_tests PRIVATE cxx_std_20)
    welllog_enable_warnings(welllog_depth_transform_overlay_tests)
    target_link_libraries(
        welllog_depth_transform_overlay_tests
        PRIVATE WellLog::ExportVector WellLog::Session
    )
    add_test(NAME welllog.depth-transform-overlay
             COMMAND welllog_depth_transform_overlay_tests)

    # Arrow adapter tests need Arrow C++ for IPC fixtures + ExportArray round-trip.
    add_executable(welllog_observability_tests
        tests/integration/observability_test.cpp
    )
    target_compile_features(welllog_observability_tests PRIVATE cxx_std_20)
    welllog_enable_warnings(welllog_observability_tests)
    target_link_libraries(
        welllog_observability_tests
        PRIVATE WellLog::Session
    )
    add_test(NAME welllog.observability COMMAND welllog_observability_tests)

    add_executable(welllog_container_security_tests
        tests/integration/container_security_test.cpp
    )
    target_compile_features(welllog_container_security_tests PRIVATE cxx_std_20)
    welllog_enable_warnings(welllog_container_security_tests)
    target_link_libraries(
        welllog_container_security_tests
        PRIVATE WellLog::IO
    )
    add_test(NAME welllog.container-security
             COMMAND welllog_container_security_tests)

    # Deterministic corpus + mutation fuzz (#172). Always runs under CTest;
    # raise iterations with WELLLOG_FUZZ_ITERS. Optional ASan/UBSan via host
    # toolchain (WELLLOG_SANITIZERS / CXXFLAGS).
    set(WELLLOG_FUZZ_CORPUS_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/corpus")
    add_executable(welllog_fuzz_binary_sources_tests
        tests/fuzz/fuzz_binary_sources_test.cpp
    )
    target_compile_features(welllog_fuzz_binary_sources_tests PRIVATE cxx_std_20)
    welllog_enable_warnings(welllog_fuzz_binary_sources_tests)
    target_compile_definitions(
        welllog_fuzz_binary_sources_tests
        PRIVATE "WELLLOG_FUZZ_CORPUS_DIR=\"${WELLLOG_FUZZ_CORPUS_DIR}\""
    )
    target_include_directories(
        welllog_fuzz_binary_sources_tests
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz"
    )
    target_link_libraries(
        welllog_fuzz_binary_sources_tests
        PRIVATE WellLog::IO
    )
    add_test(NAME welllog.fuzz-binary-sources
             COMMAND welllog_fuzz_binary_sources_tests)

    add_executable(welllog_fuzz_assets_tests
        tests/fuzz/fuzz_assets_test.cpp
    )
    target_compile_features(welllog_fuzz_assets_tests PRIVATE cxx_std_20)
    welllog_enable_warnings(welllog_fuzz_assets_tests)
    target_compile_definitions(
        welllog_fuzz_assets_tests
        PRIVATE "WELLLOG_FUZZ_CORPUS_DIR=\"${WELLLOG_FUZZ_CORPUS_DIR}\""
    )
    target_include_directories(
        welllog_fuzz_assets_tests
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz"
    )
    target_link_libraries(
        welllog_fuzz_assets_tests
        PRIVATE WellLog::IO WellLog::Scene
    )
    add_test(NAME welllog.fuzz-assets COMMAND welllog_fuzz_assets_tests)

    if(WELLLOG_BUILD_ARROW AND WELLLOG_ARROW_HAS_IPC)
        add_executable(welllog_arrow_adapter_tests
            tests/integration/arrow_adapter_test.cpp
        )
        target_compile_features(welllog_arrow_adapter_tests PRIVATE cxx_std_20)
        welllog_enable_warnings(welllog_arrow_adapter_tests)
        target_link_libraries(
            welllog_arrow_adapter_tests
            PRIVATE
                WellLog::Arrow
                WellLog::Session
                WellLog::Scene
                Arrow::arrow_shared
        )
        add_test(NAME welllog.arrow-adapter COMMAND welllog_arrow_adapter_tests)
    endif()

    # Export parity compares SVG/PDF with glyph outlines; needs TEXT (#234).
    if(WELLLOG_BUILD_TEXT)
        add_executable(welllog_export_parity_tests
            tests/integration/export_parity_test.cpp
        )
        target_compile_features(welllog_export_parity_tests PRIVATE cxx_std_20)
        welllog_enable_warnings(welllog_export_parity_tests)
        target_compile_definitions(
            welllog_export_parity_tests
            PRIVATE
                "WELLLOG_TEST_FONT_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/assets/fonts\""
        )
        target_include_directories(
            welllog_export_parity_tests
            PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
        )
        target_link_libraries(
            welllog_export_parity_tests
            PRIVATE WellLog::ExportVector WellLog::ExportPdf WellLog::Scene WellLog::Text WellLog::Session ZLIB::ZLIB
        )
        add_test(NAME welllog.export-parity COMMAND welllog_export_parity_tests)
    endif()

    add_executable(welllog_table_projection_tests
        tests/integration/table_projection_test.cpp
    )
    target_compile_features(welllog_table_projection_tests PRIVATE cxx_std_20)
    welllog_enable_warnings(welllog_table_projection_tests)
    target_link_libraries(welllog_table_projection_tests PRIVATE WellLog::Table)
    add_test(NAME welllog.table-projection COMMAND welllog_table_projection_tests)

    # CSV table-export writer (ADR 0022 / table-and-export.md §7, #155).
    # Headless: links only ExportTable (Core+Table); no GL or Qt.
    add_executable(welllog_csv_table_export_tests
        tests/integration/csv_table_export_test.cpp
    )
    target_compile_features(welllog_csv_table_export_tests PRIVATE cxx_std_20)
    welllog_enable_warnings(welllog_csv_table_export_tests)
    target_link_libraries(welllog_csv_table_export_tests PRIVATE WellLog::ExportTable)
    add_test(NAME welllog.csv-table-export COMMAND welllog_csv_table_export_tests)

    # Composite/Chained BufferView (#196, foundation for #162 append). Headless;
    # core-only — the type lives in core beside BufferView.
    add_executable(welllog_composite_buffer_view_tests
        tests/integration/composite_buffer_view_test.cpp
    )
    target_compile_features(welllog_composite_buffer_view_tests PRIVATE cxx_std_20)
    welllog_enable_warnings(welllog_composite_buffer_view_tests)
    target_link_libraries(welllog_composite_buffer_view_tests PRIVATE WellLog::Core)
    add_test(NAME welllog.composite-buffer-view COMMAND welllog_composite_buffer_view_tests)

    # Composite-buffer consumer migration (#197): a multi-segment curve flows
    # through LOD + table projection/exporters identically to single-block.
    add_executable(welllog_composite_curve_migration_tests
        tests/integration/composite_curve_migration_test.cpp
    )
    target_compile_features(welllog_composite_curve_migration_tests PRIVATE cxx_std_20)
    welllog_enable_warnings(welllog_composite_curve_migration_tests)
    target_link_libraries(
        welllog_composite_curve_migration_tests
        PRIVATE WellLog::Scene WellLog::Table WellLog::ExportTable
    )
    target_include_directories(
        welllog_composite_curve_migration_tests
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME welllog.composite-curve-migration COMMAND welllog_composite_curve_migration_tests)

    # Versioned XML table-export writer (§6, #155). Headless; ExportTable only.
    add_executable(welllog_xml_table_export_tests
        tests/integration/xml_table_export_test.cpp
    )
    target_compile_features(welllog_xml_table_export_tests PRIVATE cxx_std_20)
    welllog_enable_warnings(welllog_xml_table_export_tests)
    target_link_libraries(welllog_xml_table_export_tests PRIVATE WellLog::ExportTable)
    add_test(NAME welllog.xml-table-export COMMAND welllog_xml_table_export_tests)

    # Self-contained XLSX (OOXML+zlib) table export (§5, #155). Headless; reads
    # the archive back via ZLIB inflate (no Excel dependency).
    add_executable(welllog_xlsx_table_export_tests
        tests/integration/xlsx_table_export_test.cpp
    )
    target_compile_features(welllog_xlsx_table_export_tests PRIVATE cxx_std_20)
    welllog_enable_warnings(welllog_xlsx_table_export_tests)
    target_link_libraries(
        welllog_xlsx_table_export_tests
        PRIVATE WellLog::ExportTable ZLIB::ZLIB
    )
    add_test(NAME welllog.xlsx-table-export COMMAND welllog_xlsx_table_export_tests)

    add_executable(welllog_viewport_tests
        tests/integration/viewport_interaction_test.cpp
    )
    target_compile_features(welllog_viewport_tests PRIVATE cxx_std_20)
    welllog_enable_warnings(welllog_viewport_tests)
    target_link_libraries(welllog_viewport_tests PRIVATE WellLog::Session)
    add_test(NAME welllog.viewport COMMAND welllog_viewport_tests)

    # Shared semantic Selection Set in WellLogSession (ADR 0024, #154 Phase B).
    # Headless: selection maps Reference Depth Range ↔ row span and remaps/
    # invalidates on document replacement; no GL or Qt needed.
    add_executable(welllog_session_selection_tests
        tests/integration/session_selection_test.cpp
    )
    target_compile_features(welllog_session_selection_tests PRIVATE cxx_std_20)
    welllog_enable_warnings(welllog_session_selection_tests)
    target_link_libraries(welllog_session_selection_tests PRIVATE WellLog::Session)
    add_test(NAME welllog.session-selection COMMAND welllog_session_selection_tests)

    # AppendBatchCommand (#198, ADR 0031): atomic curve tail-append producing a
    # new Document Revision, with no copy of the old data blocks; rejects
    # out-of-order/backfill tails and non-monotonic revisions. Headless.
    add_executable(welllog_append_batch_tests
        tests/integration/append_batch_test.cpp
    )
    target_compile_features(welllog_append_batch_tests PRIVATE cxx_std_20)
    welllog_enable_warnings(welllog_append_batch_tests)
    target_link_libraries(welllog_append_batch_tests PRIVATE WellLog::Session)
    add_test(NAME welllog.append-batch COMMAND welllog_append_batch_tests)

    # Incremental LOD tail-extension (#199, ADR 0031): extend_tail reuses the
    # unchanged earlier region's summaries and re-derives only the tail, with
    # parity to a full rebuild. Headless; Scene only.
    add_executable(welllog_incremental_lod_tests
        tests/integration/incremental_lod_test.cpp
    )
    target_compile_features(welllog_incremental_lod_tests PRIVATE cxx_std_20)
    welllog_enable_warnings(welllog_incremental_lod_tests)
    target_link_libraries(welllog_incremental_lod_tests PRIVATE WellLog::Scene)
    add_test(NAME welllog.incremental-lod COMMAND welllog_incremental_lod_tests)

    # Session append-incremental LOD (#199): a session that appends a tail
    # (incremental extend_tail worker) produces a Prepared Scene LOD-identical
    # to a session that directly submits the full curve (full build). Headless;
    # Session only (dense curve drives the async worker).
    add_executable(welllog_append_incremental_session_tests
        tests/integration/append_incremental_session_test.cpp
    )
    target_compile_features(welllog_append_incremental_session_tests PRIVATE cxx_std_20)
    welllog_enable_warnings(welllog_append_incremental_session_tests)
    target_link_libraries(welllog_append_incremental_session_tests PRIVATE WellLog::Session)
    add_test(NAME welllog.append-incremental-session COMMAND welllog_append_incremental_session_tests)

    # Session append viewport mode (#200): Fixed preserves the viewport across
    # an append, Follow-Latest advances the bottom to the new tail depth. Head.
    add_executable(welllog_append_viewport_mode_tests
        tests/integration/append_viewport_mode_test.cpp
    )
    target_compile_features(welllog_append_viewport_mode_tests PRIVATE cxx_std_20)
    welllog_enable_warnings(welllog_append_viewport_mode_tests)
    target_link_libraries(welllog_append_viewport_mode_tests PRIVATE WellLog::Session)
    add_test(NAME welllog.append-viewport-mode COMMAND welllog_append_viewport_mode_tests)

    # High-frequency append coalescing + stress/robustness (#201, final #162
    # criterion): coalescing cap, external-owner retention, append-LOD cancel,
    # selection survival, rapid-append pressure. Headless; Session + core.
    add_executable(welllog_append_coalescing_tests
        tests/integration/append_coalescing_test.cpp
    )
    target_compile_features(welllog_append_coalescing_tests PRIVATE cxx_std_20)
    welllog_enable_warnings(welllog_append_coalescing_tests)
    target_link_libraries(welllog_append_coalescing_tests PRIVATE WellLog::Session)
    add_test(NAME welllog.append-coalescing-stress COMMAND welllog_append_coalescing_tests)

    # DocumentPatch + ApplyPatchCommand (#202, the #158 undoable-patch
    # foundation): upsert/remove on document + presentation entities, atomic
    # whole-batch reject, patch_conflict on stale base, selection remap.
    # Headless; Session + Scene.
    add_executable(welllog_apply_patch_tests
        tests/integration/apply_patch_test.cpp
    )
    target_compile_features(welllog_apply_patch_tests PRIVATE cxx_std_20)
    welllog_enable_warnings(welllog_apply_patch_tests)
    target_link_libraries(welllog_apply_patch_tests PRIVATE WellLog::Session)
    add_test(NAME welllog.apply-patch COMMAND welllog_apply_patch_tests)

    # Interpretation-entity patch coverage (#205, AC #2 of #158): create/move/
    # modify/delete of Interval/Marker/Annotation via ApplyPatchCommand, with
    # invalid-edit rejections (Interval top>=bottom, invalid UTF-8) and
    # prepared-scene reflection. Headless; Session + Scene.
    add_executable(welllog_interpretation_patch_tests
        tests/integration/interpretation_patch_test.cpp
    )
    target_compile_features(welllog_interpretation_patch_tests PRIVATE cxx_std_20)
    welllog_enable_warnings(welllog_interpretation_patch_tests)
    target_link_libraries(welllog_interpretation_patch_tests PRIVATE WellLog::Session)
    add_test(NAME welllog.interpretation-patch COMMAND welllog_interpretation_patch_tests)

    # Kernel-owned per-document undo/redo history (#203, ADR 0025): patch and
    # append semantic round trips, redo clearing, Selection Set restoration,
    # and can_undo/can_redo + history_changed observability. Headless.
    add_executable(welllog_undo_redo_tests
        tests/integration/undo_redo_test.cpp
    )
    target_compile_features(welllog_undo_redo_tests PRIVATE cxx_std_20)
    welllog_enable_warnings(welllog_undo_redo_tests)
    target_link_libraries(welllog_undo_redo_tests PRIVATE WellLog::Session)
    add_test(NAME welllog.undo-redo COMMAND welllog_undo_redo_tests)

    # End-to-end seam validation for #206 / #158 AC #8: after a document
    # patch, undo and redo, graphics, table, SVG and ViewEvent outputs must
    # agree on one semantic revision. Headless; no GL or Qt adapter needed.
    add_executable(welllog_session_seam_validation_tests
        tests/integration/session_seam_validation_test.cpp
    )
    target_compile_features(
        welllog_session_seam_validation_tests
        PRIVATE cxx_std_20
    )
    welllog_enable_warnings(welllog_session_seam_validation_tests)
    target_link_libraries(
        welllog_session_seam_validation_tests
        PRIVATE WellLog::ExportVector WellLog::Table WellLog::Session
    )
    add_test(
        NAME welllog.session-seam-validation
        COMMAND welllog_session_seam_validation_tests
    )

    add_executable(welllog_render_gl_tests
        tests/render_gl/capability_test.cpp
    )
    target_compile_features(welllog_render_gl_tests PRIVATE cxx_std_20)
    welllog_enable_warnings(welllog_render_gl_tests)
    target_include_directories(
        welllog_render_gl_tests
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(
        welllog_render_gl_tests
        PRIVATE WellLog::RenderGL
    )
    add_test(NAME welllog.render-gl COMMAND welllog_render_gl_tests)

    add_executable(welllog_raster_tests
        tests/render_gl/raster_test.cpp
    )
    target_compile_features(welllog_raster_tests PRIVATE cxx_std_20)
    welllog_enable_warnings(welllog_raster_tests)
    target_include_directories(
        welllog_raster_tests
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(welllog_raster_tests PRIVATE WellLog::RenderGL)
    add_test(NAME welllog.render-raster COMMAND welllog_raster_tests)

    if(WELLLOG_BUILD_QT_WIDGETS)
        qt_add_executable(welllog_qt_widget_tests
            tests/qt/well_log_view_test.cpp
        )
        welllog_enable_warnings(welllog_qt_widget_tests)
        # Qt Test macros (QTRY_*_WITH_TIMEOUT) convert chrono::milliseconds::rep
        # (long) to int under -Wconversion; the macros are third-party.
        if(NOT MSVC)
            target_compile_options(
                welllog_qt_widget_tests
                PRIVATE -Wno-conversion -Wno-sign-conversion
            )
        endif()
        target_link_libraries(
            welllog_qt_widget_tests
            PRIVATE WellLog::QtWidgets Qt6::Test
        )
        add_test(NAME welllog.qt-widget COMMAND welllog_qt_widget_tests)
        add_test(
            NAME welllog.qt-unavailable
            COMMAND
                welllog_qt_widget_tests
                unavailable_context_publishes_a_capability_report
        )
        set_tests_properties(
            welllog.qt-unavailable
            PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=minimal"
        )
        if(UNIX AND NOT APPLE)
            set_tests_properties(
                welllog.qt-widget
                PROPERTIES ENVIRONMENT "LIBGL_ALWAYS_SOFTWARE=1"
            )
        endif()

        # #173 — create/reparent/destroy churn, multi-view isolation, Trace toggle.
        qt_add_executable(welllog_qt_context_lifecycle_stress_tests
            tests/qt/context_lifecycle_stress_test.cpp
        )
        welllog_enable_warnings(welllog_qt_context_lifecycle_stress_tests)
        if(NOT MSVC)
            target_compile_options(
                welllog_qt_context_lifecycle_stress_tests
                PRIVATE -Wno-conversion -Wno-sign-conversion
            )
        endif()
        target_link_libraries(
            welllog_qt_context_lifecycle_stress_tests
            PRIVATE WellLog::QtWidgets Qt6::Test
        )
        add_test(
            NAME welllog.qt-context-lifecycle-stress
            COMMAND welllog_qt_context_lifecycle_stress_tests
        )
        if(UNIX AND NOT APPLE)
            set_tests_properties(
                welllog.qt-context-lifecycle-stress
                PROPERTIES ENVIRONMENT "LIBGL_ALWAYS_SOFTWARE=1"
            )
        endif()

        # Virtualized table-model adapter test (ADR 0022, #154). No GL needed
        # (a table model has no OpenGL) — runs under the minimal platform.
        qt_add_executable(welllog_qt_table_model_tests
            tests/qt/table_model_test.cpp
        )
        welllog_enable_warnings(welllog_qt_table_model_tests)
        target_link_libraries(
            welllog_qt_table_model_tests
            PRIVATE WellLog::QtWidgets Qt6::Test
        )
        add_test(NAME welllog.qt-table-model COMMAND welllog_qt_table_model_tests)
        set_tests_properties(
            welllog.qt-table-model
            PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=minimal"
        )

        # Bidirectional graphics↔table selection sync (ADR 0024, #154 Phase B).
        # No GL needed — a TableModel + headless session only.
        qt_add_executable(welllog_qt_table_selection_sync_tests
            tests/qt/table_selection_sync_test.cpp
        )
        welllog_enable_warnings(welllog_qt_table_selection_sync_tests)
        target_link_libraries(
            welllog_qt_table_selection_sync_tests
            PRIVATE WellLog::QtWidgets WellLog::Session WellLog::Table Qt6::Test
        )
        add_test(
            NAME welllog.qt-table-selection-sync
            COMMAND welllog_qt_table_selection_sync_tests
        )
        set_tests_properties(
            welllog.qt-table-selection-sync
            PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=minimal"
        )
    endif()
    if(WELLLOG_BUILD_PYTHON)
        # Controlled runtime (E1/E2): the python tests run the configured
        # interpreter with the build's welllog package AND the interpreter's
        # own PySide6/shiboken site-packages — never ambient shell PYTHONPATH.
        add_test(
            NAME welllog.python.qt-embedding
            COMMAND
                "${CMAKE_COMMAND}" -E env
                "PYTHONPATH=${CMAKE_CURRENT_BINARY_DIR}/python:${WELLLOG_PYTHON_SITE_PACKAGES}"
                "QT_QPA_PLATFORM=minimal"
                "${Python_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/python/test_qt_embedding.py"
        )
        add_test(
            NAME welllog.python.qt-lifecycle-stress
            COMMAND
                "${CMAKE_COMMAND}" -E env
                "PYTHONPATH=${CMAKE_CURRENT_BINARY_DIR}/python:${WELLLOG_PYTHON_SITE_PACKAGES}"
                "QT_QPA_PLATFORM=minimal"
                "${Python_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/python/test_qt_lifecycle_stress.py"
        )
    endif()

    add_test(
        NAME welllog.package.consumer
        COMMAND
            "${CMAKE_COMMAND}"
            "-DENGINE_BUILD_DIR=${CMAKE_BINARY_DIR}"
            "-DCONSUMER_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/tests/consumer"
            "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/package-consumer"
            "-DTEST_CONFIG=$<CONFIG>"
            "-DENGINE_GENERATOR=${CMAKE_GENERATOR}"
            "-DENGINE_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
            "-DENGINE_GENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
            "-DENGINE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
            "-DENGINE_CXX_FLAGS=${CMAKE_CXX_FLAGS}"
            "-DENGINE_EXE_LINKER_FLAGS=${CMAKE_EXE_LINKER_FLAGS}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/consumer/run_consumer_test.cmake"
    )
    add_test(
        NAME welllog.svg.package.consumer
        COMMAND
            "${CMAKE_COMMAND}"
            "-DENGINE_BUILD_DIR=${CMAKE_BINARY_DIR}"
            "-DCONSUMER_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/tests/svg_consumer"
            "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/svg-package-consumer"
            "-DTEST_CONFIG=$<CONFIG>"
            "-DENGINE_GENERATOR=${CMAKE_GENERATOR}"
            "-DENGINE_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
            "-DENGINE_GENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
            "-DENGINE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
            "-DENGINE_CXX_FLAGS=${CMAKE_CXX_FLAGS}"
            "-DENGINE_EXE_LINKER_FLAGS=${CMAKE_EXE_LINKER_FLAGS}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/consumer/run_consumer_test.cmake"
    )
    if(WELLLOG_BUILD_QT_WIDGETS)
        add_test(
            NAME welllog.qt.package.consumer
            COMMAND
                "${CMAKE_COMMAND}"
                "-DENGINE_BUILD_DIR=${CMAKE_BINARY_DIR}"
                "-DCONSUMER_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/tests/qt_consumer"
                "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/qt-package-consumer"
                "-DTEST_CONFIG=$<CONFIG>"
                "-DENGINE_GENERATOR=${CMAKE_GENERATOR}"
                "-DENGINE_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
                "-DENGINE_GENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
                "-DENGINE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
                "-DENGINE_CXX_FLAGS=${CMAKE_CXX_FLAGS}"
                "-DENGINE_EXE_LINKER_FLAGS=${CMAKE_EXE_LINKER_FLAGS}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/consumer/run_consumer_test.cmake"
        )
    endif()
    add_test(
        NAME welllog.core.dependency-boundary
        COMMAND
            "${CMAKE_COMMAND}"
            "-DCORE_INCLUDE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/include/welllog/core"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/check_core_boundary.cmake"
    )

    # #174 — aggregate label for publish-gate subset (ctest -L release-gate).
    # Absolute frame P95 is NOT in this set (ref HW only; QSP §4.3).
    set(_welllog_release_gate_tests
        welllog.release-gate-scenario
        welllog.dense-curve-pipeline
        welllog.async-lrw-stress
        welllog.manifest
        welllog.container-security
        welllog.package.consumer
        welllog.svg.package.consumer
        welllog.fuzz-binary-sources
        welllog.fuzz-assets
        welllog.raster-export
        welllog.export-parity
    )
    foreach(_t IN LISTS _welllog_release_gate_tests)
        if(TEST ${_t})
            set_property(TEST ${_t} APPEND PROPERTY LABELS release-gate)
        endif()
    endforeach()
    if(TEST welllog.qt-context-lifecycle-stress)
        set_property(
            TEST welllog.qt-context-lifecycle-stress
            APPEND PROPERTY LABELS release-gate
        )
    endif()
endif()

