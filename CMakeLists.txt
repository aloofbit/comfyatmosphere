# 32-bit only: the 1.12 client is x86, so the DLL loaded into it must be too.
#   cmake -B build -A Win32 && cmake --build build --config Release
cmake_minimum_required(VERSION 3.15)
# CMP0091 must be NEW before project() for CMAKE_MSVC_RUNTIME_LIBRARY to take effect.
cmake_policy(SET CMP0091 NEW)

# Static CRT: the DLL is dropped into a game folder that may have no VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(comfyfog CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(comfyfog SHARED
    src/comfyfog.cpp
    src/config.cpp
    src/rays.cpp
    src/beams.cpp
    src/timeofday.cpp
    src/depth.cpp
    src/shadow.cpp
    src/client.cpp
    src/volume.cpp
)

# comfyfog.dll, loaded by VanillaFixes from dlls.txt, same as comfygrass. It exports nothing.
# The DLL lands in the project root next to comfyfog.ini; the empty $<0:> stops MSVC adding a
# per-config subfolder.
set_target_properties(comfyfog PROPERTIES
    OUTPUT_NAME "comfyfog"
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}$<0:>")

if(MSVC)
    target_compile_options(comfyfog PRIVATE /W3 /permissive- /GS-)
endif()
