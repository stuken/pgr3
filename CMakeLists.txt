# pgr3 - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(pgr3 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Embedded window icon: extracted directly from assets/default.xex (already a
# required codegen input) so no extra asset needs to be checked in or fetched.
# src/icon.generated.h is a gitignored build artifact regenerated whenever the
# XEX or the generator script changes.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(PGR3_ICON_HEADER ${CMAKE_SOURCE_DIR}/src/icon.generated.h)
set(PGR3_ICON_ICO ${CMAKE_SOURCE_DIR}/src/icon.generated.ico)
add_custom_command(
    OUTPUT ${PGR3_ICON_HEADER} ${PGR3_ICON_ICO}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/gen-icon.py
    DEPENDS
        ${CMAKE_SOURCE_DIR}/scripts/gen-icon.py
        ${CMAKE_SOURCE_DIR}/assets/default.xex
    COMMENT "Generating src/icon.generated.h and src/icon.generated.ico"
    VERBATIM
)

# On Windows, also embed the same icon as the .exe's own resource (Explorer,
# taskbar before the window opens, Alt-Tab), separate from the in-window icon
# applied at runtime via Window::SetIcon (see PGR3_app.h). The .rc
# needs an absolute path to the .ico since it's not necessarily emitted next
# to it in an out-of-source build.
if(WIN32)
    set(PGR3_ICON_ICO_PATH ${PGR3_ICON_ICO})
    set(PGR3_ICON_RC ${CMAKE_SOURCE_DIR}/src/PGR3.generated.rc)
    configure_file(
        ${CMAKE_SOURCE_DIR}/src/PGR3.rc.in
        ${PGR3_ICON_RC}
        @ONLY
    )
    # rc.exe needs icon.generated.ico to exist by the time it compiles this
    # .rc; configure_file only stamped the path into it above, it doesn't
    # establish a build-order dependency on the custom command that (re)writes
    # the .ico itself.
    set_source_files_properties(${PGR3_ICON_RC} PROPERTIES OBJECT_DEPENDS ${PGR3_ICON_ICO})
endif()

# Sources
set(PGR3_SOURCES
    src/main.cpp
    ${PGR3_ICON_HEADER}
)

if(WIN32)
    add_executable(pgr3 WIN32 ${PGR3_SOURCES} ${PGR3_ICON_RC})
else()
    add_executable(pgr3 ${PGR3_SOURCES})
endif()

rexglue_setup_target(pgr3 GPU_PLUGINS xenos)
