cmake_minimum_required(VERSION 3.20)
project(NBAStreetHomecourtPC CXX C)

# Needs Clang (clang-cl on Windows): the generated code uses Clang builtins.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

set(XENONRECOMP_DIR "${CMAKE_SOURCE_DIR}/thirdparty/XenonRecomp" CACHE PATH "XenonRecomp checkout")
add_subdirectory(${XENONRECOMP_DIR} xenonrecomp EXCLUDE_FROM_ALL)

# --- recompiled game code (ppc/ is produced by XenonRecomp from config/nbash.toml)
file(GLOB PPC_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/ppc/ppc_recomp.*.cpp" "${CMAKE_SOURCE_DIR}/ppc/ppc_func_mapping.cpp")
add_library(NBASHGame STATIC ${PPC_SOURCES})
target_include_directories(NBASHGame PUBLIC "${CMAKE_SOURCE_DIR}/ppc" "${XENONRECOMP_DIR}/thirdparty/simde")
if(MSVC)
    target_compile_options(NBASHGame PRIVATE /FI${CMAKE_SOURCE_DIR}/runtime/indirect.h)
else()
    target_compile_options(NBASHGame PRIVATE -include ${CMAKE_SOURCE_DIR}/runtime/indirect.h)
endif()
if(MSVC)
    target_compile_options(NBASHGame PRIVATE /clang:-O1 -mavx2 -Wno-everything)
else()
    target_compile_options(NBASHGame PRIVATE -O1 -march=x86-64-v3 -fno-strict-aliasing -Wno-everything)
endif()

# --- runtime
add_executable(nbash
    runtime/main.cpp
    runtime/memory.cpp
    runtime/loader.cpp
    runtime/kernel.cpp
    runtime/gpu.cpp
    runtime/input.cpp
    runtime/audio.cpp
    runtime/xma.cpp
    runtime/profiler.cpp
    runtime/loopstats.cpp
    runtime/actiontrace.cpp
    runtime/gameplay.cpp
    runtime/music.cpp
    runtime/mods.cpp
    runtime/archivemod.cpp
    runtime/posprobe.cpp
    runtime/statscreen.cpp
    runtime/content.cpp
    runtime/window.cpp
    runtime/stubs.cpp)
target_include_directories(nbash PRIVATE runtime "${XENONRECOMP_DIR}/XenonUtils")
target_link_libraries(nbash PRIVATE NBASHGame XenonUtils)
if(WIN32)
    # renderer: Xenos shaders -> HLSL (Xenia's ucode decoder, BSD) + Direct3D 11
    target_sources(nbash PRIVATE
        runtime/render/hlsl_translator.cpp
        runtime/render/renderer_d3d11.cpp
        thirdparty/xenia/xenia/gpu/shader.cc
        thirdparty/xenia/xenia/gpu/shader_translator.cc
        thirdparty/xenia/xenia/gpu/shader_translator_disasm.cc
        thirdparty/xenia/xenia/gpu/registers.cc
        thirdparty/xenia/xenia/gpu/ucode.cc
        thirdparty/xenia/xenia/gpu/xenos.cc)
    target_include_directories(nbash PRIVATE thirdparty/xenia)
    target_link_libraries(nbash PRIVATE d3d11 dxgi)
    target_compile_options(nbash PRIVATE -march=x86-64-v3)
    # XMA audio: FFmpeg's xmaframes decoder (Xenia's FFmpeg fork, static, decoder only)
    target_include_directories(nbash PRIVATE thirdparty/ffmpeg-win/include)
    target_compile_definitions(nbash PRIVATE NBASH_HAVE_FFMPEG)
    target_link_libraries(nbash PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/ffmpeg-win/lib/libavcodec.a
        ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/ffmpeg-win/lib/libavutil.a bcrypt)
    target_link_libraries(nbash PRIVATE winmm mfplat mfreadwrite mfuuid ole32 propsys shell32 uuid -static)
    target_link_options(nbash PRIVATE -mwindows -Wl,--stack,16777216)
elseif(MSVC)
    target_compile_options(nbash PRIVATE -mavx2)
else()
    target_compile_options(nbash PRIVATE -march=x86-64-v3)
    find_package(Threads REQUIRED)
    target_link_libraries(nbash PRIVATE Threads::Threads)
endif()
