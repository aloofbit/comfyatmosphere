// The server's weather, read from its packet (2026-10-12, the owner: the clouds waited for the drops, "waiting for the
// droplets seems weak"). The clouds took the rain from the game's rain draws (water.cpp, WaterNoteRain), so they
// gathered only once the drops showed, and cleared only once the drops had gone. The server sends SMSG_WEATHER
// (0x2F4) when a zone's weather changes and when a player enters a zone: a uint32 type (0 fine, 1 rain, 2 snow,
// 3 storm), a float grade 0..1, a uint32 sound and a uint8 (1 an instant change). The game then fades its drops in.
//
// The client registers one handler for many opcodes (0x0048F690, by SetMessageHandler 0x005AB650 at 0x0048F55B):
// __fastcall (param in ecx, opcode in edx; the time and the packet, a CDataStore, on the stack). Its first six bytes
// (push ebp; mov ebp, esp; sub esp, 0x34) go to a stub that reads the packet when edx is 0x2F4, without moving its
// read position, then runs them and goes back. The bytes are checked first: another mod's patch there is left alone.
// CDataStore: +0x04 the buffer, +0x10 the size, +0x14 the read position (after the opcode).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <cstring>

#include "weather.h"
#include "common.h"

namespace
{
    constexpr uintptr_t kHandler = 0x0048F690;
    constexpr uint8_t   kEntry[6] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x34 };
    constexpr DWORD     kOpcode = 0x2F4;

    DWORD    g_resume  = 0;       // the handler past its first six bytes
    bool     g_tried   = false;
    bool     g_patched = false;
    bool     g_have    = false;   // a packet has come
    int      g_type    = 0;
    float    g_grade   = 0.0f;
    unsigned g_count   = 0;
    unsigned g_bad     = 0;

    bool SafeCopy(uintptr_t src, void* dst, size_t n)
    {
#ifdef _MSC_VER
        __try
        {
            memcpy(dst, reinterpret_cast<const void*>(src), n);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
#else
        // MinGW has no __try: the system reads the memory for us and fails where it is not readable.
        SIZE_T got = 0;
        return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(src), dst, n, &got) && got == n;
#endif
    }

    bool WriteCode(uintptr_t at, const uint8_t* code, DWORD len)
    {
        DWORD prot = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(at), len, PAGE_EXECUTE_READWRITE, &prot))
            return false;
        memcpy(reinterpret_cast<void*>(at), code, len);
        VirtualProtect(reinterpret_cast<void*>(at), len, prot, &prot);
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(at), len);
        return true;
    }

    uintptr_t Site()
    {
        const intptr_t slide = reinterpret_cast<intptr_t>(GetModuleHandleW(nullptr)) - 0x00400000;
        return static_cast<uintptr_t>(static_cast<intptr_t>(kHandler) + slide);
    }

    void __stdcall OnWeather(uintptr_t store)
    {
        uintptr_t buf = 0;
        uint32_t size = 0, read = 0;
        if (!store || !SafeCopy(store + 0x04, &buf, 4) || !SafeCopy(store + 0x10, &size, 4) ||
            !SafeCopy(store + 0x14, &read, 4))
            return;
        uint32_t type = 0;
        float grade = 0.0f;
        // 13 bytes after the opcode: the packet as the server writes it (Weather::SendWeatherUpdateToPlayer).
        if (!buf || read > size || size - read != 13 || !SafeCopy(buf + read, &type, 4) ||
            !SafeCopy(buf + read + 4, &grade, 4) || type > 3 || !(grade >= 0.0f && grade <= 1.0f))
        {
            if (g_bad++ < 4)
                Log("weather: a weather packet not as expected (size %u, read at %u, type %u, grade %.3f): left out",
                    size, read, type, grade);
            return;
        }
        static const char* const kNames[] = { "fine", "rain", "snow", "a storm" };
        if (!g_have || static_cast<int>(type) != g_type || grade != g_grade)
            Log("weather: the server sends %s, grade %.2f", kNames[type], grade);
        g_type  = static_cast<int>(type);
        g_grade = grade;
        g_have  = true;
        ++g_count;
    }

#ifdef _MSC_VER
    // At the handler's entry: [esp] the return, [esp+4] the time, [esp+8] the packet.
    __declspec(naked) void Stub()
    {
        __asm
        {
            cmp   edx, kOpcode
            jne   original
            pushad
            mov   eax, [esp + 32 + 8]
            push  eax
            call  OnWeather
            popad
        original:
            push  ebp
            mov   ebp, esp
            sub   esp, 0x34
            jmp   dword ptr [g_resume]
        }
    }

#else
    // The same stub in GCC's syntax (MinGW). OnWeather is __stdcall: it takes its argument off the stack.
    __attribute__((naked)) void Stub()
    {
        asm volatile(
            "cmpl  %0, %%edx\n\t"
            "jne   1f\n\t"
            "pushal\n\t"
            "movl  40(%%esp), %%eax\n\t"
            "pushl %%eax\n\t"
            "call  %P1\n\t"
            "popal\n"
            "1:\n\t"
            "pushl %%ebp\n\t"
            "movl  %%esp, %%ebp\n\t"
            "subl  $0x34, %%esp\n\t"
            "jmp   *%2\n\t"
            :: "i"(kOpcode), "i"(&OnWeather), "m"(g_resume));
    }
#endif

    void Install()
    {
        g_tried = true;
        const uintptr_t site = Site();
        uint8_t have[sizeof(kEntry)] = {};
        if (!SafeCopy(site, have, sizeof(have)) || memcmp(have, kEntry, sizeof(kEntry)) != 0)
        {
            Log("weather: the packet handler at 0x%08X is not as expected (%02X %02X %02X %02X %02X %02X): not patched, "
                "the clouds follow the rain drawn", static_cast<DWORD>(site), have[0], have[1], have[2], have[3],
                have[4], have[5]);
            return;
        }
        g_resume = static_cast<DWORD>(site + sizeof(kEntry));
        uint8_t code[sizeof(kEntry)] = { 0xE9, 0, 0, 0, 0, 0x90 };
        const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&Stub) - (site + 5));
        memcpy(code + 1, &rel, sizeof(rel));
        if (!WriteCode(site, code, sizeof(code)))
        {
            Log("weather: VirtualProtect failed (%lu), the packet handler is not patched", GetLastError());
            return;
        }
        g_patched = true;
        Log("weather: the packet handler at 0x%08X patched: the clouds follow the server's weather",
            static_cast<DWORD>(site));
    }
}

void WeatherFrameEnd()
{
    if (!g_tried)
        Install();
}

void WeatherDetach()
{
    if (!g_patched)
        return;
    if (WriteCode(Site(), kEntry, sizeof(kEntry)))
    {
        g_patched = false;
        Log("weather: the packet handler's patch is taken out");
    }
    else
        Log("weather: VirtualProtect failed (%lu), the packet handler's patch stays", GetLastError());
}

bool WeatherServer(int& type, float& grade)
{
    if (!g_have)
        return false;
    type  = g_type;
    grade = g_grade;
    return true;
}

void WeatherProbe()
{
    static const char* const kNames[] = { "fine", "rain", "snow", "a storm" };
    Log("weather: the packet handler %s; %u weather packets, %u left out; %s", g_patched ? "patched" :
        g_tried ? "NOT patched" : "not patched yet", g_count, g_bad,
        g_have ? kNames[g_type] : "no weather from the server yet (the clouds follow the rain drawn)");
    if (g_have)
        Log("weather: the last from the server: %s, grade %.2f", kNames[g_type], g_grade);
}
