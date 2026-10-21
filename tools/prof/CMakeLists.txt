# comfyprof.dll: a sampling profiler for the 1.12 client's threads (2026-10-20). Test clients only.
# 32-bit: the 1.12 client is x86.
#   cmake -B build -A Win32 && cmake --build build --config Release
cmake_minimum_required(VERSION 3.15)
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(comfyprof CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(comfyprof SHARED src/prof.cpp)
target_link_libraries(comfyprof PRIVATE psapi winmm dbghelp)
set_target_properties(comfyprof PROPERTIES
    OUTPUT_NAME "comfyprof"
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}$<0:>")

if(MSVC)
    target_compile_options(comfyprof PRIVATE /W3 /permissive- /GS- /utf-8 /O2)
endif()
