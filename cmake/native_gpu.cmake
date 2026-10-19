# The native renderer (PGR3_NATIVE_GPU): rr6's plume-based Xenos translation
# layer, fed by the XenosRecomp shader cache, in place of the SDK's emulated
# GPU. The guest's D3D library is to be replaced by REX_HOOKs in src/gpu/hooks.
# Ported from rr6's cmake/native_gpu.cmake.
#
# Expects the pgr3 target (already through rexglue_setup_target),
# PGR3_SOURCES, PGR3_COMMON_SOURCES, PGR3_ICON_RC, PGR3_GEN_DIR,
# PGR3_SHADER_COMMON_H and the pgr3_shader_cache library to exist already.
enable_language(C)

cmake_dependent_option(PGR3_D3D12
    "Build pgr3 with the D3D12 backend (OFF selects Vulkan)" ON "WIN32" OFF)
cmake_dependent_option(PGR3_VULKAN_EXE
    "Build pgr3_vk, the Vulkan backend, alongside the D3D12 pgr3" ON
    "WIN32;PGR3_D3D12" OFF)

set(PGR3_D3D12_TARGETS "")
set(PGR3_VULKAN_TARGETS "")
if(PGR3_D3D12)
    list(APPEND PGR3_D3D12_TARGETS pgr3)
else()
    list(APPEND PGR3_VULKAN_TARGETS pgr3)
endif()

if(PGR3_VULKAN_EXE)
    add_executable(pgr3_vk WIN32 ${PGR3_SOURCES} ${PGR3_ICON_RC})
    target_sources(pgr3_vk PRIVATE ${PGR3_COMMON_SOURCES})
    target_include_directories(pgr3_vk PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    # The recompiled guest code never reads the backend, so both exes link the
    # pgr3_recomp objects rexglue_setup_target made for pgr3 instead of
    # compiling them a second time. The macro only adds them while this list is
    # set.
    set(pgr3_recomp_sources "${REXGLUE_ENTRYPOINT_GENERATED_SOURCES}")
    set(REXGLUE_ENTRYPOINT_GENERATED_SOURCES "")
    rexglue_setup_target(pgr3_vk)
    set(REXGLUE_ENTRYPOINT_GENERATED_SOURCES "${pgr3_recomp_sources}")
    if(TARGET pgr3_recomp)
        target_link_libraries(pgr3_vk PRIVATE pgr3_recomp)
    endif()
    # The SDK stages the same runtime DLLs next to every exe it configures, so
    # serialize the links to keep those POST_BUILD copies off each other.
    add_dependencies(pgr3_vk pgr3)
    list(APPEND PGR3_VULKAN_TARGETS pgr3_vk)
endif()
set(PGR3_NATIVE_TARGETS ${PGR3_D3D12_TARGETS} ${PGR3_VULKAN_TARGETS})

# Linux picks X11 or Wayland at runtime, so plume builds its surface through
# the SDL3 window the SDK already owns (rexglue.cmake has imported SDL3 by now).
if(LINUX)
    set(PLUME_SDL_VULKAN_ENABLED ON CACHE BOOL "" FORCE)
endif()
add_subdirectory(thirdparty/plume EXCLUDE_FROM_ALL)
include(cmake/shaders.cmake)

file(GLOB_RECURSE pgr3_native_sources CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/*.cpp")
# Built from the top-level CMakeLists: PGR3_COMMON_SOURCES for both renderers,
# and the census and resolve dump for the plugin build only.
foreach(source IN LISTS PGR3_COMMON_SOURCES ITEMS
        src/platform/shader_census.cpp src/platform/xenos_resolve_dump.cpp)
    list(REMOVE_ITEM pgr3_native_sources "${CMAKE_CURRENT_SOURCE_DIR}/${source}")
endforeach()
# The runtime DXC shader linker has no Vulkan counterpart: SPIR-V takes the
# spec constants as Vulkan specialization constants instead.
set(pgr3_d3d12_sources
    "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/shaders/dxc_link.cpp")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/shaders/shader_linker.cpp")
    list(APPEND pgr3_d3d12_sources
        "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/shaders/shader_linker.cpp")
endif()
list(REMOVE_ITEM pgr3_native_sources ${pgr3_d3d12_sources})
set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/miniz/miniz.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/zstd/zstddeclib.c"
    PROPERTIES COMPILE_OPTIONS "-w")

foreach(shader IN ITEMS copy_vs imgui_vs)
    pgr3_host_shader(${shader} vs_6_0)
endforeach()
foreach(shader IN ITEMS
        copy_color_ps copy_depth_ps debug_solid_ps fxaa_ps gamma_correction_ps
        pfx_occlusion_count_ps imgui_ps)
    pgr3_host_shader(${shader} ps_6_0)
endforeach()

foreach(target IN LISTS PGR3_NATIVE_TARGETS)
    target_sources(${target} PRIVATE
        ${pgr3_native_sources}
        "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/miniz/miniz.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/zstd/zstddeclib.c")
    # The recompiler's defines (thirdparty/CMakeLists.txt), so host code reads
    # the same shader_common.h tables (vertex input locations) as the shaders.
    target_compile_definitions(${target} PRIVATE
        PGR3_NATIVE_GPU REBLUE_RECOMP PGR3_RECOMP)
    target_include_directories(${target} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${PGR3_GEN_DIR}"
        "${PGR3_SHADER_COMMON_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/miniz"
        "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/zstd")
    target_link_libraries(${target} PRIVATE
        plume
        pgr3_shader_cache
        rex::xxhash
        rex::o1heap)
endforeach()

if(PGR3_D3D12_TARGETS)
    # pgr3_prelink DXC-links every specConstantsMask subset of the shader cache
    # at build time, so the runtime never pays a DXC link for a shipped shader.
    add_executable(pgr3_prelink
        tools/prelink_shader_cache.cpp
        src/gpu/shaders/dxc_link.cpp
        "${PGR3_GEN_DIR}/shader_cache.cpp"
        thirdparty/miniz/miniz.cpp
        thirdparty/zstd/zstddeclib.c)
    target_include_directories(pgr3_prelink PRIVATE
        src thirdparty/miniz thirdparty/zstd)
    target_link_libraries(pgr3_prelink PRIVATE Microsoft::DirectXShaderCompiler)
    pgr3_stage_dxc(pgr3_prelink)

    add_custom_command(
        OUTPUT "${PGR3_GEN_DIR}/linked_shader_cache.cpp"
        COMMAND pgr3_prelink "${PGR3_GEN_DIR}/linked_shader_cache.cpp"
        DEPENDS pgr3_prelink
        COMMENT "Pre-linking spec-constant shader variants"
        VERBATIM)
endif()

foreach(target IN LISTS PGR3_D3D12_TARGETS)
    target_sources(${target} PRIVATE
        ${pgr3_d3d12_sources}
        "${PGR3_GEN_DIR}/linked_shader_cache.cpp")
    target_compile_definitions(${target} PRIVATE PGR3_D3D12)
    target_link_libraries(${target} PRIVATE
        d3d12 dxgi
        Microsoft::DirectXShaderCompiler)
    pgr3_stage_dxc(${target})
endforeach()

# The SPIR-V cache stores each module smol-v encoded.
set(pgr3_smolv_dir "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/XenosRecomp/thirdparty/smol-v/source")
set_source_files_properties("${pgr3_smolv_dir}/smolv.cpp" PROPERTIES COMPILE_OPTIONS "-w")
foreach(target IN LISTS PGR3_VULKAN_TARGETS)
    target_sources(${target} PRIVATE "${pgr3_smolv_dir}/smolv.cpp")
    target_include_directories(${target} PRIVATE "${pgr3_smolv_dir}")
endforeach()

# On Windows plume builds VK_KHR_win32_surface straight off the HWND and
# reaches vulkan-1.dll through volk, so the Vulkan backend links nothing extra.
if(NOT WIN32)
    foreach(target IN LISTS PGR3_VULKAN_TARGETS)
        find_package(Vulkan REQUIRED)
        target_link_libraries(${target} PRIVATE Vulkan::Vulkan)
    endforeach()
endif()
