// Exercise the actual time controller with a fake wall clock and client-time
// storage in this executable. No game process, graphics device or real clock
// changes. Only INI parsing uses a temporary file.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include "../src/config.h"

static SYSTEMTIME localClock = {};
static bool otherTimeDll = false;
static double elapsed = 0;
static unsigned writes = 0;
static bool failWrite = false;
static std::map<std::string, std::string> written;

static void TestGetLocalTime(SYSTEMTIME* value) { *value = localClock; }
static HMODULE TestGetModuleHandleA(LPCSTR name)
{
    if (std::string(name) == "comfytime.dll")
        return otherTimeDll ? reinterpret_cast<HMODULE>(1) : nullptr;
    return GetModuleHandleA(name);
}
#define GetLocalTime TestGetLocalTime
#define GetModuleHandleA TestGetModuleHandleA
#include "../src/timeofday.cpp"
#undef GetModuleHandleA
#undef GetLocalTime

void Log(const char*, ...) {}
double Now() { return elapsed; }
bool TuneWriteIni(const std::map<std::string, std::string>& values, std::string& error)
{
    ++writes;
    if (failWrite) { error = "test write failure"; return false; }
    written = values;
    return true;
}

static void Check(bool condition, const char* message)
{
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
static bool Near(float a, float b) { return std::fabs(a - b) < 0.0002f; }
struct ClientClock { unsigned minutes; float fraction; float minuteFloat; };
static ClientClock clocks[16];
static unsigned clockIndex = 0;
static ClientClock& Setup()
{
    static_assert(sizeof(void*) == 4, "Build this client-time test as Win32");
    Check(clockIndex < 16, "test clock capacity");
    ClientClock& clock = clocks[clockIndex++];
    clock = {720, 0.5f, 720.0f};
    g_cfg = Settings{};
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    g_cfg.time.addrMinutes = static_cast<DWORD>(reinterpret_cast<uintptr_t>(&clock.minutes) - base + 0x400000);
    g_cfg.time.addrFraction = static_cast<DWORD>(reinterpret_cast<uintptr_t>(&clock.fraction) - base + 0x400000);
    g_cfg.time.addrMinutesF = static_cast<DWORD>(reinterpret_cast<uintptr_t>(&clock.minuteFloat) - base + 0x400000);
    elapsed = 0;
    writes = 0;
    written.clear();
    failWrite = otherTimeDll = false;
    TimeAttach();
    TimeReload();
    return clock;
}
static void SetClock(WORD hour, WORD minute, WORD second = 0, WORD milliseconds = 0)
{
    localClock = {};
    localClock.wHour = hour;
    localClock.wMinute = minute;
    localClock.wSecond = second;
    localClock.wMilliseconds = milliseconds;
}
static void ReadWrittenMode()
{
    if (written.count("time.enabled")) g_cfg.time.enabled = written.at("time.enabled") == "1";
    if (written.count("time.localTime")) g_cfg.time.localTime = written.at("time.localTime") == "1";
    if (written.count("time.hour")) g_cfg.time.hour = std::stof(written.at("time.hour"));
    TimeReload();
}

static void ConfigTest()
{
    WCHAR directory[MAX_PATH], file[MAX_PATH];
    Check(GetTempPathW(MAX_PATH, directory) != 0, "temporary directory");
    Check(GetTempFileNameW(directory, L"cat", 0, file) != 0, "temporary INI");
    LoadSettings(file);
    Check(g_cfg.time.enabled && !g_cfg.time.localTime, "existing default remains fixed time");
    Check(WritePrivateProfileStringW(L"time", L"localTime", L"1", file) != 0, "write local mode");
    LoadSettings(file);
    Check(g_cfg.time.localTime, "parse localTime INI setting");
    ConfigOverrides()["time.localTime"] = "0";
    LoadSettings(file);
    Check(!g_cfg.time.localTime, "runtime tuning can override local mode");
    ConfigOverrides().clear();
    Check(DeleteFileW(file) != 0, "remove temporary INI");
}

static void ModesTest()
{
    ClientClock& clock = Setup();
    TimeApply("test");
    Check(clock.minutes == 780 && TimeState() == 1, "legacy fixed hour still applies");
    TimeSet(23.5f);
    TimeStep(1.0f);
    Check(Near(TimeCurrentHour(), 0.5f), "manual stepping crosses midnight");
    TimeToggleDayNight();
    Check(Near(TimeCurrentHour(), 13.0f), "manual day/night toggle still works");
    elapsed = 2;
    TimePersistTick();
    Check(writes == 1 && written.count("time.hour"), "manual changes still persist");

    SetClock(22, 15, 30);
    Check(TimeLocalWrite(true), "select local mode");
    Check(!written.count("time.hour"), "selecting local mode preserves saved fixed hour");
    ReadWrittenMode();
    TimeApply("test");
    Check(TimeState() == 4 && clock.minutes == 1335, "local mode writes local time");
    Check(Near(TimeCurrentHour(), 22.258333f), "seconds included");
    const unsigned before = writes;
    TimeSet(4);
    TimeStep(1);
    TimeToggleDayNight();
    elapsed += 10;
    TimePersistTick();
    Check(writes == before && Near(TimeCurrentHour(), 22.258333f), "manual controls do not move or persist local clock");

    SetClock(7, 45);
    TimeApply("test");
    Check(clock.minutes == 465, "clock corrections and resume are sampled directly");
    const std::string handover = TimeHandOver();
    SetClock(9, 30);
    TimeTakeOver(handover);
    TimeApply("test");
    Check(clock.minutes == 570, "hot reload does not freeze an old local hour");
    Check(TimeLockWrite(true), "switch local clock to fixed time");
    ReadWrittenMode();
    SetClock(10, 30);
    TimeApply("test");
    Check(!g_cfg.time.localTime && Near(TimeCurrentHour(), 9.5f), "Lock time captures current local hour");
    Check(TimeLocalWrite(false), "return to server mode");
    ReadWrittenMode();
    clock = {720, 0.5f, 720.0f};
    TimeApply("test");
    Check(TimeState() == 0 && clock.minutes == 720, "server mode does not write client clock");
}

static void MidnightTest()
{
    ClientClock& clock = Setup();
    g_cfg.time.localTime = true;
    TimeAfterTune();
    SetClock(23, 59, 59, 999);
    TimeApply("test");
    Check(TimeCurrentHour() < 24 && clock.minutes == 1439 && clock.fraction < 1,
        "last millisecond remains a valid day fraction");
    SetClock(0, 0);
    TimeApply("test");
    Check(clock.minutes == 0 && clock.fraction == 0, "midnight wraps naturally");
    elapsed = 10;
    TimePersistTick();
    Check(writes == 0, "ticking local clock does not write the INI");
}

static void DstTest()
{
    // Use Windows' Madrid timezone rules without changing the host timezone.
    DYNAMIC_TIME_ZONE_INFORMATION madrid = {};
    bool found = false;
    for (DWORD i = 0; EnumDynamicTimeZoneInformation(i, &madrid) == ERROR_SUCCESS; ++i)
        if (std::wstring(madrid.TimeZoneKeyName) == L"Romance Standard Time") { found = true; break; }
    Check(found, "Madrid timezone available");
    madrid.DynamicDaylightTimeDisabled = FALSE;
    ClientClock& clock = Setup();
    g_cfg.time.localTime = true;
    TimeAfterTune();
    struct Case { WORD month, day, hour, minute; unsigned expected; };
    for (const Case& c : { Case{3,29,0,59,119}, Case{3,29,1,0,180},
                          Case{10,25,0,59,179}, Case{10,25,1,0,120} })
    {
        SYSTEMTIME utc = {};
        utc.wYear = 2026; utc.wMonth = c.month; utc.wDay = c.day;
        utc.wHour = c.hour; utc.wMinute = c.minute;
        Check(SystemTimeToTzSpecificLocalTimeEx(&madrid, &utc, &localClock) != 0, "convert DST boundary");
        TimeApply("test");
        Check(clock.minutes == c.expected, "follow spring-forward and fall-back clock readings");
    }
}

static void GuardsTest()
{
    ClientClock& clock = Setup();
    g_cfg.time.localTime = true;
    g_cfg.time.enabled = false;
    TimeReload();
    SetClock(20, 30);
    TimeApply("test");
    Check(TimeState() == 0 && clock.minutes == 720, "enabled switch still wins");
    g_cfg.time.enabled = true;
    clock.fraction = 0.2f;
    TimeReload();
    TimeApply("test");
    Check(TimeState() == 3 && clock.minutes == 720, "invalid client-time addresses/data refused");
    failWrite = true;
    Check(!TimeLocalWrite(true), "INI write failure reported");
    otherTimeDll = true;
    TimeAttach();
    TimeApply("test");
    Check(TimeState() == 2 && clock.minutes == 720 && !TimeLocalWrite(true), "old time DLL conflict retained");
}

int main()
{
    ConfigTest(); ModesTest(); MidnightTest(); DstTest(); GuardsTest();
    std::puts("PASS: INI/overrides, three modes, manual controls, persistence, midnight, Madrid DST, clock changes, hot reload, guards");
}
