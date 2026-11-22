# Building on Linux

## What changed

- **comfyatmos.dll builds with MinGW.** `cmake/mingw32.cmake` is a toolchain file for `i686-w64-mingw32-g++`;
  configure with it and `-G Ninja`, as in the README. The DLL is 32-bit, links the C++ runtime in, as MSVC's static
  CRT does, and exports only `ComfyHotDetach`, as the MSVC build.
- **What MinGW cannot take, and what stands in for it.** Each is behind `#ifndef _MSC_VER`, so the Visual Studio
  build compiles exactly what it did before:
  - `__try`/`__except` round reads of the client's memory (`SafeCopy`, `SafeRead`, `SafeString`, `ReadDetailModel`)
    become `ReadProcessMemory`, which fails where the memory is not readable. `SafeWrite` writes only into
    committed, writable pages, checked with `VirtualQuery`, as the guarded copy would.
  - The three naked stubs of the grass and weather patches are written again in GCC's syntax; disassembled, they
    are the same instructions as the MSVC ones.
  - A function's address passed as `void*` (`HookSlot`, `WriteJump`) goes through a cast.
  - The libraries MSVC takes from `#pragma comment(lib)` are named in `CMakeLists.txt`.
- **Both builds on every push** (`.github/workflows/build.yml`): Visual Studio on Windows, MinGW on Linux, with the
  DLLs as artifacts. The MinGW side hides nothing from MSVC, so a push shows at once if the Windows build breaks.

## Why

Under Wine the client and the DLL run on Linux, and a Linux machine can now build the DLL it runs, with no Windows
and no Visual Studio.
