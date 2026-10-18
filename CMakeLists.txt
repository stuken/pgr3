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
# applied at runtime via Window::SetIcon (see pgr3_app.h). The .rc
# needs an absolute path to the .ico since it's not necessarily emitted next
# to it in an out-of-source build.
if(WIN32)
    set(PGR3_ICON_ICO_PATH ${PGR3_ICON_ICO})
    set(PGR3_ICON_RC ${CMAKE_SOURCE_DIR}/src/pgr3.generated.rc)
    configure_file(
        ${CMAKE_SOURCE_DIR}/src/pgr3.rc.in
        ${PGR3_ICON_RC}
        @ONLY
    )
    # rc.exe needs icon.generated.ico to exist by the time it compiles this
    # .rc; configure_file only stamped the path into it above, it doesn't
    # establish a build-order dependency on the custom command that (re)writes
    # the .ico itself.
    set_source_files_properties(${PGR3_ICON_RC} PROPERTIES OBJECT_DEPENDS ${PGR3_ICON_ICO})
endif()

# Xenos shaders: every shader PGR3 creates is embedded in default.xex, and the
# title update (default.xexp) replaces some of them, so they are extracted from
# the image as the SDK loads it, patch applied. pgr3_dump_image loads it the
# way codegen does and writes it out; scripts/extract_shaders.py then writes
# one .vso/.pso per unique container into generated/shaders, including the
# four the game builds on its stack from instruction immediates. The XEX header
# gives the image's load address.
set(PGR3_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(PGR3_IMAGE "${PGR3_GEN_DIR}/default.image")
set(PGR3_SHADER_DIR "${PGR3_GEN_DIR}/shaders")

add_executable(pgr3_dump_image tools/dump_image.cpp)
target_link_libraries(pgr3_dump_image PRIVATE rex::runtime)
rexglue_apply_target_settings(pgr3_dump_image)
if(WIN32)
    add_custom_command(TARGET pgr3_dump_image POST_BUILD
        COMMAND "$<$<BOOL:$<TARGET_RUNTIME_DLLS:pgr3_dump_image>>:${CMAKE_COMMAND};-E;copy_if_different;$<TARGET_RUNTIME_DLLS:pgr3_dump_image>;$<TARGET_FILE_DIR:pgr3_dump_image>>"
        COMMAND_EXPAND_LISTS
        VERBATIM)
endif()

set(PGR3_IMAGE_INPUTS "${CMAKE_SOURCE_DIR}/assets/default.xex")
if(EXISTS "${CMAKE_SOURCE_DIR}/assets/default.xexp")
    list(APPEND PGR3_IMAGE_INPUTS "${CMAKE_SOURCE_DIR}/assets/default.xexp")
endif()
add_custom_command(
    OUTPUT "${PGR3_IMAGE}"
    COMMAND pgr3_dump_image "${CMAKE_SOURCE_DIR}/assets" default.xex "${PGR3_IMAGE}"
    DEPENDS pgr3_dump_image ${PGR3_IMAGE_INPUTS}
    COMMENT "Loading default.xex with its title update into ${PGR3_IMAGE}"
    VERBATIM)
add_custom_command(
    OUTPUT "${PGR3_SHADER_DIR}/shaders.csv"
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/scripts/extract_shaders.py"
        --image "${PGR3_IMAGE}" "${CMAKE_SOURCE_DIR}/assets/default.xex" "${PGR3_SHADER_DIR}"
    DEPENDS "${CMAKE_SOURCE_DIR}/scripts/extract_shaders.py" "${PGR3_IMAGE}"
        "${CMAKE_SOURCE_DIR}/assets/default.xex"
    COMMENT "Extracting Xenos shaders from the patched image"
    VERBATIM)
add_custom_target(pgr3_shaders ALL DEPENDS "${PGR3_SHADER_DIR}/shaders.csv")

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

# Test tooling shared by both renderers: scripted pad input
# (pgr3_input_script), silent audio (pgr3_null_audio) and the abort logger.
target_sources(pgr3 PRIVATE
    src/core/memory_helpers.cpp
    src/core/settings.cpp
    src/platform/guest_abort_log.cpp
    src/platform/input_script.cpp)
target_include_directories(pgr3 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

rexglue_setup_target(pgr3 GPU_PLUGINS xenos)
# Resolve-target dumps and frame traces, comparable with the native renderer's,
# and the shader census (pgr3_shader_census), which wraps the Create*Shader
# functions the native renderer will replace.
target_sources(pgr3 PRIVATE
    src/platform/shader_census.cpp
    src/platform/xenos_resolve_dump.cpp)
