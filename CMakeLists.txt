cmake_minimum_required(VERSION 3.20)
project(WaveRace64Recomp VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# =============================================================================
# Options
# =============================================================================
option(BUILD_RT64 "Build RT64 rendering backend" ON)

message(STATUS "WaveRace64-Recomp v${PROJECT_VERSION}")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")

# Opt out of constexpr mutex constructor on windows to prevent vcredist issues
if (WIN32)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR")
    # Enable the resource compiler so the custom application icon
    # (resources/wr64.rc -> resources/wr64.ico) is embedded in the exe.
    enable_language(RC)
elseif (APPLE)
    # RT64/plume's Metal backend has Objective-C++ (.mm) sources. The
    # language must be enabled at the TOP level — rt64's own
    # enable_language(OBJC OBJCXX) is scoped to its subdirectory and does not
    # propagate up, so CMake's generate step fails with a missing
    # CMAKE_OBJCXX_COMPILE_OBJECT. Matches Zelda64Recomp/BanjoRecomp.
    enable_language(OBJC OBJCXX)
endif()

# =============================================================================
# Find / fetch SDL2 early — must happen before subdirectories that use it
# (recompinput reads sdl2_SOURCE_DIR at configure time)
# =============================================================================
if (NOT WIN32)
    find_package(SDL2 REQUIRED)
else()
    include(FetchContent)

    if (DEFINED ENV{SDL2_VERSION})
        set(SDL2_VERSION $ENV{SDL2_VERSION})
    else()
        set(SDL2_VERSION "2.30.3")
    endif()

    FetchContent_Declare(
        sdl2
        URL https://github.com/libsdl-org/SDL/releases/download/release-${SDL2_VERSION}/SDL2-devel-${SDL2_VERSION}-VC.zip
    )
    FetchContent_MakeAvailable(sdl2)

    # Mirror SDL2 headers into an SDL2/ subdirectory so both <SDL.h> and
    # <SDL2/SDL.h> include styles resolve.
    file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/sdl2-compat-include/SDL2)
    file(GLOB SDL2_COMPAT_HEADERS ${sdl2_SOURCE_DIR}/include/*.h)
    file(COPY ${SDL2_COMPAT_HEADERS} DESTINATION ${CMAKE_BINARY_DIR}/sdl2-compat-include/SDL2)
endif()

# =============================================================================
# RT64 — static rendering backend
# =============================================================================
if (BUILD_RT64)
    set(RT64_STATIC ON CACHE BOOL "Build RT64 as a static library" FORCE)
    set(RT64_SDL_WINDOW_VULKAN ON CACHE BOOL "RT64 uses SDL Vulkan window" FORCE)
    add_subdirectory(lib/rt64 EXCLUDE_FROM_ALL)
endif()

# =============================================================================
# N64ModernRuntime (ultramodern + librecomp)
# =============================================================================
add_subdirectory(lib/N64ModernRuntime EXCLUDE_FROM_ALL)

# =============================================================================
# RecompFrontend (optional launcher UI — submodule at lib/RecompFrontend)
# =============================================================================
set(RECOMP_FRONTEND_PATH "" CACHE PATH "Path to RecompFrontend repository")
if (NOT RECOMP_FRONTEND_PATH)
    set(_builtin_rf "${CMAKE_CURRENT_SOURCE_DIR}/lib/RecompFrontend")
    if (EXISTS "${_builtin_rf}/CMakeLists.txt")
        set(RECOMP_FRONTEND_PATH "${_builtin_rf}" CACHE PATH "Path to RecompFrontend repository" FORCE)
    endif()
endif()

# Re-export RT64 shader-compiler variables into the parent scope so that
# subdirectories added after lib/rt64 (e.g. RecompFrontend) can see them.
# CMake normal variables set inside add_subdirectory do NOT propagate back.
if (BUILD_RT64)
    set(_rt64_src "${CMAKE_CURRENT_SOURCE_DIR}/lib/rt64/src")
    # Non-Windows dxc is a portable binary that needs its bundled
    # libdxcompiler next to it at RUNTIME — rt64's own CMake prefixes the
    # command with LD_LIBRARY_PATH/DYLD_LIBRARY_PATH (custom commands run via
    # sh, so a leading VAR=... token works); mirror that here or the
    # RecompFrontend shader rules fail with "libdxcompiler.so: cannot open".
    if (WIN32)
        set(DXC "${_rt64_src}/contrib/dxc/bin/x64/dxc.exe")
    elseif (APPLE)
        if (CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
            set(DXC "DYLD_LIBRARY_PATH=${_rt64_src}/contrib/dxc/lib/arm64" "${_rt64_src}/contrib/dxc/bin/arm64/dxc-macos")
        else()
            set(DXC "DYLD_LIBRARY_PATH=${_rt64_src}/contrib/dxc/lib/x64" "${_rt64_src}/contrib/dxc/bin/x64/dxc-macos")
        endif()
    else()
        if (CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
            set(DXC "LD_LIBRARY_PATH=${_rt64_src}/contrib/dxc/lib/arm64" "${_rt64_src}/contrib/dxc/bin/arm64/dxc-linux")
        else()
            set(DXC "LD_LIBRARY_PATH=${_rt64_src}/contrib/dxc/lib/x64" "${_rt64_src}/contrib/dxc/bin/x64/dxc-linux")
        endif()
    endif()
    set(DXC_COMMON_OPTS "-I${_rt64_src}")
    set(DXC_DXIL_OPTS   "-Wno-ignored-attributes")
    set(DXC_SPV_OPTS    "-spirv" "-fspv-target-env=vulkan1.0" "-fvk-use-dx-layout")
    set(DXC_VS_OPTS     ${DXC_COMMON_OPTS} "-E" "VSMain" "-T" "vs_6_3" "-fvk-invert-y")
    set(DXC_PS_OPTS     ${DXC_COMMON_OPTS} "-E" "PSMain" "-T" "ps_6_3")
    set(DXC_CS_OPTS     ${DXC_COMMON_OPTS} "-E" "CSMain" "-T" "cs_6_3")
    set(DXC_GS_OPTS     ${DXC_COMMON_OPTS} "-E" "GSMain" "-T" "gs_6_3")
endif()

if (RECOMP_FRONTEND_PATH)
    message(STATUS "RecompFrontend: ${RECOMP_FRONTEND_PATH}")
    set(RECOMP_FRONTEND_N64MODERNRUNTIME_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lib/N64ModernRuntime" CACHE PATH "" FORCE)
    set(RECOMP_FRONTEND_RT64_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lib/rt64" CACHE PATH "" FORCE)
    add_subdirectory(${RECOMP_FRONTEND_PATH} ${CMAKE_BINARY_DIR}/recompfrontend EXCLUDE_FROM_ALL)
    # rt64 enables PLUME_SDL_VULKAN_ENABLED / RT64_SDL_WINDOW_VULKAN via
    # directory-scoped add_compile_definitions, which recompui (added from a
    # different directory) does NOT inherit. Without them plume::RenderWindow
    # is the raw X11 {Display*, Window} pair on Linux while ultramodern's
    # WindowHandle is an SDL_Window*, and recompui's renderer fails to
    # compile ("no viable overloaded '='"). Mirror the defines here.
    if (BUILD_RT64 AND CMAKE_SYSTEM_NAME MATCHES "Linux" AND RT64_SDL_WINDOW_VULKAN AND TARGET recompui)
        target_compile_definitions(recompui PRIVATE PLUME_SDL_VULKAN_ENABLED RT64_SDL_WINDOW_VULKAN)
    endif()
else()
    message(STATUS "RecompFrontend: not found (set RECOMP_FRONTEND_PATH to enable launcher UI)")
endif()

# =============================================================================
# Recompiled functions — static library from N64Recomp output
# =============================================================================
file(GLOB RECOMP_FUNC_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/RecompiledFuncs/funcs_*.c"
)

add_library(recomp_funcs STATIC
    ${RECOMP_FUNC_SOURCES}
    "${CMAKE_CURRENT_SOURCE_DIR}/RecompiledFuncs/lookup.cpp"
)

target_include_directories(recomp_funcs PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/RecompiledFuncs"
)

# recomp_funcs needs access to recomp.h (for recomp_context, gpr, etc.)
target_include_directories(recomp_funcs PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/N64Recomp/include"
)

# Suppress warnings in generated recompiled code.
target_compile_options(recomp_funcs PRIVATE
    -Wno-unused-variable
    -Wno-unused-parameter
    -Wno-sign-compare
    -Wno-unused-but-set-variable
    -Wno-implicit-function-declaration
)

# =============================================================================
# Game executable
# =============================================================================
# =============================================================================
# Game patches: patches/*.c -> MIPS elf -> recompiled native code
# =============================================================================
if(NOT DEFINED PATCHES_C_COMPILER)
    set(PATCHES_C_COMPILER clang)
endif()
if(NOT DEFINED PATCHES_LD)
    set(PATCHES_LD ld.lld)
endif()

# Build the patches elf with the MIPS cross toolchain.
add_custom_target(PatchesBin
    COMMAND ${CMAKE_COMMAND} -E env CC=${PATCHES_C_COMPILER} LD=${PATCHES_LD} make
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/patches
    BYPRODUCTS ${CMAKE_CURRENT_SOURCE_DIR}/patches/patches.elf
)

# Recompile the patches elf into native code + the patch data binary.
add_custom_command(OUTPUT
                       ${CMAKE_CURRENT_SOURCE_DIR}/patches/patches.bin
                       ${CMAKE_CURRENT_SOURCE_DIR}/RecompiledPatches/patches.c
                       ${CMAKE_CURRENT_SOURCE_DIR}/RecompiledPatches/recomp_overlays.inl
                       ${CMAKE_CURRENT_SOURCE_DIR}/RecompiledPatches/funcs.h
                   COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/lib/N64Recomp/build/N64Recomp patches.toml
                   WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                   DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/patches/patches.elf
)

# Embed the patch data binary as a C array.
add_custom_command(OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/RecompiledPatches/patches_bin.c
    COMMAND file_to_c ${CMAKE_CURRENT_SOURCE_DIR}/patches/patches.bin mm_patches_bin ${CMAKE_CURRENT_SOURCE_DIR}/RecompiledPatches/patches_bin.c ${CMAKE_CURRENT_SOURCE_DIR}/RecompiledPatches/patches_bin.h
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/patches/patches.bin
)

add_library(PatchesLib STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/RecompiledPatches/patches.c
    ${CMAKE_CURRENT_SOURCE_DIR}/RecompiledPatches/patches_bin.c
)
add_dependencies(PatchesLib PatchesBin)

target_include_directories(PatchesLib PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/N64Recomp/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/N64ModernRuntime/librecomp/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/N64ModernRuntime/ultramodern/include"
)

set(WR64_SOURCES
    src/main.cpp
    src/rt64_render_context.cpp
    src/rsp_microcode.cpp
    src/audio.cpp
    src/input.cpp
    src/os_stubs.cpp
    src/register_patches.cpp
    rsp/aspMain.cpp
)

# Embed the Windows application icon.
if (WIN32)
    list(APPEND WR64_SOURCES resources/wr64.rc)
    # llvm-rc's dependency scan doesn't see the .ico referenced inside the .rc,
    # so regenerating wr64.ico alone won't retrigger the resource compile. Make
    # the dependency explicit so the icon is always re-embedded when it changes.
    set_source_files_properties(resources/wr64.rc PROPERTIES
        OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/resources/wr64.ico")
endif()

add_executable(WaveRace64Recomp ${WR64_SOURCES})

target_include_directories(WaveRace64Recomp PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/RecompiledFuncs"
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/N64Recomp/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/N64ModernRuntime/librecomp/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/N64ModernRuntime/ultramodern/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/N64ModernRuntime/thirdparty"
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/N64ModernRuntime/thirdparty/concurrentqueue"
)

# RT64 include paths (for rt64_application.h, etc.)
if (BUILD_RT64)
    target_include_directories(WaveRace64Recomp PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/lib/rt64/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/lib/rt64/src/contrib"
        "${CMAKE_CURRENT_SOURCE_DIR}/lib/rt64/src/contrib/hlslpp/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/lib/rt64/src/contrib/plume"
        "${CMAKE_CURRENT_SOURCE_DIR}/lib/rt64/src/contrib/imgui"
        "${CMAKE_CURRENT_SOURCE_DIR}/lib/rt64/src/contrib/mupen64plus-core/src/api"
    )
    target_compile_definitions(WaveRace64Recomp PRIVATE HAS_RT64=1 HLSL_CPU FFX_GCC)
endif()

if (RECOMP_FRONTEND_PATH)
    target_compile_definitions(WaveRace64Recomp PRIVATE HAS_RECOMPUI=1)
    target_include_directories(WaveRace64Recomp PRIVATE
        "${RECOMP_FRONTEND_PATH}/recompui/include"
        "${RECOMP_FRONTEND_PATH}/recompui/src"
        "${RECOMP_FRONTEND_PATH}/recompinput/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/lib/rt64/src/contrib/nativefiledialog-extended/src/include"
    )
endif()

# Link libraries
# Use --start-group/--end-group to handle circular dependencies between
# recomp_funcs (which references librecomp symbols) and librecomp (which
# needs function tables from recomp_funcs). MSVC-style linkers resolve
# static-library cycles iteratively, so no grouping is needed there.
if (MSVC)
    target_link_libraries(WaveRace64Recomp PRIVATE
        recomp_funcs
        PatchesLib
        librecomp
        ultramodern
    )
    if (RECOMP_FRONTEND_PATH)
        target_link_libraries(WaveRace64Recomp PRIVATE recompui recompinput)
    endif()
    # Allow multiple definitions (e.g. guMtxF2L appears in multiple overlay sections)
    target_link_options(WaveRace64Recomp PRIVATE /FORCE:MULTIPLE)
else()
    target_link_libraries(WaveRace64Recomp PRIVATE
        -Wl,--start-group
        recomp_funcs
        PatchesLib
        librecomp
        ultramodern
        -Wl,--end-group
        SDL2::SDL2
    )
    if (RECOMP_FRONTEND_PATH)
        target_link_libraries(WaveRace64Recomp PRIVATE recompui recompinput)
    endif()
    # Allow multiple definitions (e.g. guMtxF2L appears in multiple overlay sections)
    target_link_options(WaveRace64Recomp PRIVATE -Wl,--allow-multiple-definition)
endif()

# Copy assets/ to the build output directory so the launcher can find fonts/rcss.
if (RECOMP_FRONTEND_PATH)
    add_custom_command(TARGET WaveRace64Recomp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/assets"
            "$<TARGET_FILE_DIR:WaveRace64Recomp>/assets"
        COMMENT "Copying launcher assets"
    )
endif()

# =============================================================================
# Windows: SDL2 target configuration and DLL copy
# (FetchContent was called early above; target-specific calls go here)
# =============================================================================
if (WIN32)
    target_include_directories(WaveRace64Recomp PRIVATE
        ${sdl2_SOURCE_DIR}/include
        ${CMAKE_BINARY_DIR}/sdl2-compat-include
    )

    # windows.h min/max macros break hlslpp (via rt64 headers)
    target_compile_definitions(WaveRace64Recomp PRIVATE NOMINMAX)
    target_link_directories(WaveRace64Recomp PRIVATE ${sdl2_SOURCE_DIR}/lib/x64)
    target_link_libraries(WaveRace64Recomp PRIVATE SDL2 Winmm.lib)

    # Copy SDL2 and dxc DLLs next to the executable
    add_custom_command(TARGET WaveRace64Recomp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${sdl2_SOURCE_DIR}/lib/x64/SDL2.dll"
            "${CMAKE_CURRENT_SOURCE_DIR}/lib/rt64/src/contrib/dxc/bin/x64/dxil.dll"
            "${CMAKE_CURRENT_SOURCE_DIR}/lib/rt64/src/contrib/dxc/bin/x64/dxcompiler.dll"
            $<TARGET_FILE_DIR:WaveRace64Recomp>)
endif()

if (BUILD_RT64)
    target_link_libraries(WaveRace64Recomp PRIVATE rt64)
endif()

# Platform-specific link flags
if (UNIX AND NOT APPLE)
    target_link_libraries(WaveRace64Recomp PRIVATE
        pthread
        dl
    )
endif()

# The recompiled RSP vector code (rsp_vu_impl.hpp) requires SSE4.1 on x86-64;
# -march=nehalem matches what Pilotwings64Recomp uses (SSE4.1-era baseline).
if(CMAKE_SIZEOF_VOID_P EQUAL 8 AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64")
    target_compile_options(WaveRace64Recomp PRIVATE -march=nehalem)
endif()

# Compiler warnings for game code
# (clang-cl/MSVC interpret -Wall as /Wall, i.e. -Weverything — far too noisy)
if (NOT MSVC)
    target_compile_options(WaveRace64Recomp PRIVATE
        -Wall
        -Wextra
        -Wno-unused-parameter
    )
endif()

# =============================================================================
# Installation
# =============================================================================
install(TARGETS WaveRace64Recomp DESTINATION bin)

# =============================================================================
# Summary
# =============================================================================
message(STATUS "RT64 backend: ${BUILD_RT64}")
message(STATUS "Recompiled function sources: ${RECOMP_FUNC_SOURCES}")
