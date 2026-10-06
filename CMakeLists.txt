# 32-bit only: the 1.12 client is x86, so the DLL loaded into it must be too.
#   cmake -B build -A Win32 && cmake --build build --config Release
cmake_minimum_required(VERSION 3.15)
# CMP0091 must be NEW before project() for CMAKE_MSVC_RUNTIME_LIBRARY to take effect.
cmake_policy(SET CMP0091 NEW)

# Static CRT: the DLL is dropped into a game folder that may have no VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(comfyatmos CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(comfyatmos SHARED
    src/comfyatmos.cpp
    src/config.cpp
    src/cvars.cpp
    src/depth.cpp
    src/shadow.cpp
    src/client.cpp
    src/volume.cpp
    src/rays.cpp
    src/bench.cpp
    src/sun.cpp
    src/bodymask.cpp
    src/cover.cpp
    src/lamps.cpp
    src/lampglow.cpp
    src/sunshadows.cpp
    src/tune.cpp
    src/terrainshade.cpp
    src/mpq.cpp
    src/mapterrain.cpp
    src/mapwmo.cpp
    src/mapm2.cpp
    src/report.cpp
    src/water.cpp
    src/grade.cpp
    src/grass.cpp
    src/beacon.cpp
    src/shadercache.cpp
)

# version.h: comfyatmos's version from git, for the client report (cmake/version.cmake).
add_custom_target(comfyatmos_version
    COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_CURRENT_SOURCE_DIR} -DOUT=${CMAKE_CURRENT_BINARY_DIR}/version.h
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.cmake
    BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/version.h)
add_dependencies(comfyatmos comfyatmos_version)
target_include_directories(comfyatmos PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

# comfyatmos.dll, loaded by VanillaFixes from dlls.txt, same as comfygrass. It exports nothing.
# The DLL lands in the project root next to comfyatmos.ini; the empty $<0:> stops MSVC adding a
# per-config subfolder.
set_target_properties(comfyatmos PROPERTIES
    OUTPUT_NAME "comfyatmos"
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}$<0:>")

if(MSVC)
    target_compile_options(comfyatmos PRIVATE /W3 /permissive- /GS-)
endif()
