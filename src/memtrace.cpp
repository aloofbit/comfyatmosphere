// memtrace: what this copy holds on the heap, by the code that allocated it (2026-10-09). Only in a build with
// -DCOMFY_MEMTRACE=ON, to find what an old copy keeps after the hot reload's detach (comfyhot.dll). Every
// operator new is counted under its first four return addresses; MemTraceReport writes the largest holders, by
// symbol, into Logs\comfyatmos-memtrace.log, which a new copy does not delete.

#include "memtrace.h"

#ifdef COMFY_MEMTRACE

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <new>

#pragma comment(lib, "dbghelp.lib")

namespace
{
    constexpr int kFrames = 4;
    constexpr int kSlots  = 1 << 15;

    struct Head                // 16 bytes, so the block keeps malloc's 8-byte alignment
    {
        size_t size;
        int    slot;
        int    pad[2];
    };

    struct Slot
    {
        void*     frames[kFrames];
        long long bytes;
        long long blocks;
        bool      used;
    };

    Slot      g_slots[kSlots];
    SRWLOCK   g_lock = SRWLOCK_INIT;
    long long g_total = 0, g_count = 0;

    int SlotFor(void* const* f)
    {
        uintptr_t h = 0;
        for (int i = 0; i < kFrames; ++i)
            h = h * 1000003u ^ reinterpret_cast<uintptr_t>(f[i]);
        for (int i = 0; i < kSlots; ++i)
        {
            Slot& s = g_slots[(h + i) & (kSlots - 1)];
            if (!s.used)
            {
                s.used = true;
                memcpy(s.frames, f, sizeof(s.frames));
                return static_cast<int>((h + i) & (kSlots - 1));
            }
            if (memcmp(s.frames, f, sizeof(s.frames)) == 0)
                return static_cast<int>((h + i) & (kSlots - 1));
        }
        return 0;   // full: all further stacks count under slot 0
    }

    void* Take(size_t n)
    {
        Head* h = static_cast<Head*>(malloc(n + sizeof(Head)));
        if (!h)
            return nullptr;
        void* f[kFrames] = {};
        RtlCaptureStackBackTrace(2, kFrames, f, nullptr);
        AcquireSRWLockExclusive(&g_lock);
        h->size = n;
        h->slot = SlotFor(f);
        g_slots[h->slot].bytes += n;
        ++g_slots[h->slot].blocks;
        g_total += n;
        ++g_count;
        ReleaseSRWLockExclusive(&g_lock);
        return h + 1;
    }

    void Give(void* p)
    {
        if (!p)
            return;
        Head* h = static_cast<Head*>(p) - 1;
        AcquireSRWLockExclusive(&g_lock);
        g_slots[h->slot].bytes -= h->size;
        --g_slots[h->slot].blocks;
        g_total -= h->size;
        --g_count;
        ReleaseSRWLockExclusive(&g_lock);
        free(h);
    }
}

void* operator new(size_t n)                                   { if (void* p = Take(n)) return p; throw std::bad_alloc(); }
void* operator new[](size_t n)                                 { if (void* p = Take(n)) return p; throw std::bad_alloc(); }
void* operator new(size_t n, const std::nothrow_t&) noexcept   { return Take(n); }
void* operator new[](size_t n, const std::nothrow_t&) noexcept { return Take(n); }
void  operator delete(void* p) noexcept                        { Give(p); }
void  operator delete[](void* p) noexcept                      { Give(p); }
void  operator delete(void* p, size_t) noexcept                { Give(p); }
void  operator delete[](void* p, size_t) noexcept              { Give(p); }

void MemTraceReport(const char* when)
{
    static Slot copy[kSlots];
    AcquireSRWLockShared(&g_lock);
    memcpy(copy, g_slots, sizeof(copy));
    const long long total = g_total, count = g_count;
    ReleaseSRWLockShared(&g_lock);

    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    wcscpy_s(wcsrchr(path, L'\\') + 1, MAX_PATH - (wcsrchr(path, L'\\') + 1 - path), L"Logs\\comfyatmos-memtrace.log");
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"a") != 0 || !f)
        return;

    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&MemTraceReport), &self);
    fprintf(f, "=== copy at %p, %s: %.1f MB live in %lld blocks ===\n", self, when, total / 1048576.0, count);

    std::sort(copy, copy + kSlots, [](const Slot& a, const Slot& b) { return a.bytes > b.bytes; });
    const HANDLE proc = GetCurrentProcess();
    static bool symInit = false;
    if (!symInit)
    {
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
        symInit = SymInitialize(proc, nullptr, TRUE) != FALSE;
    }
    for (int i = 0; i < 30 && copy[i].bytes > 0; ++i)
    {
        fprintf(f, "%8.2f MB %7lld blocks:", copy[i].bytes / 1048576.0, copy[i].blocks);
        for (int k = 0; k < kFrames; ++k)
        {
            char buf[sizeof(SYMBOL_INFO) + 256] = {};
            auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
            sym->SizeOfStruct = sizeof(SYMBOL_INFO);
            sym->MaxNameLen = 255;
            DWORD64 disp = 0;
            const DWORD64 a = reinterpret_cast<DWORD64>(copy[i].frames[k]);
            if (a && symInit && SymFromAddr(proc, a, &disp, sym))
            {
                IMAGEHLP_LINE64 line = { sizeof(line) };
                DWORD ld = 0;
                if (SymGetLineFromAddr64(proc, a, &ld, &line))
                    fprintf(f, " < %s (%s:%lu)", sym->Name, strrchr(line.FileName, '\\') ? strrchr(line.FileName, '\\') + 1 : line.FileName, line.LineNumber);
                else
                    fprintf(f, " < %s", sym->Name);
            }
            else if (a)
                fprintf(f, " < %p", copy[i].frames[k]);
        }
        fputc('\n', f);
    }
    fclose(f);
}

#endif
