// report: what this client is made of, written into comfyfog.log at start and at each F12 (2026-10-01).
//
// Players' clients differ: the DLLs VanillaFixes loads, the patch MPQs, the WoW.exe, DXVK and its conf, the
// overlays that inject themselves, the addons. A log from a player said nothing of that, and comfyfog's own
// version was not in it either. The report gives each of those, and a fingerprint line of short codes: two
// logs with the same codes come from the same client.
//
// It runs on a thread of its own, since it hashes files (WoW.exe and every DLL from outside Windows). What
// belongs to the render thread (the settings, the controls, the device) is taken before the thread starts.
// Paths under the user's profile are written as %USERPROFILE%, and Config.wtf's account lines are left out,
// so a player can post the log.

#define CINTERFACE
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <d3d9.h>
#include <tlhelp32.h>

#include "common.h"
#include "config.h"
#include "cvars.h"
#include "report.h"
#include "version.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#pragma comment(lib, "version.lib")

namespace
{
    std::mutex  g_mx;
    std::string g_device;   // the graphics card and the back buffer, from ReportDevice
    bool        g_started = false;

    // The report's lines are gathered and written in one piece, so a probe's lines, written at the same time
    // on the render thread, do not fall between them.
    thread_local std::string* t_out = nullptr;

    void Put(const char* fmt, ...)
    {
        if (!t_out)
            return;
        va_list ap;
        va_start(ap, fmt);
        const int n = _vscprintf(fmt, ap);
        va_end(ap);
        if (n <= 0)
            return;
        std::string line(static_cast<size_t>(n) + 1, '\0');
        va_start(ap, fmt);
        vsnprintf(&line[0], line.size(), fmt, ap);
        va_end(ap);
        line.resize(static_cast<size_t>(n));
        *t_out += line;
        *t_out += '\n';
    }

    std::string Narrow(const std::wstring& w)
    {
        if (w.empty())
            return {};
        const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
        std::string s(n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
        return s;
    }

    std::wstring Lower(std::wstring s)
    {
        for (wchar_t& c : s)
            c = static_cast<wchar_t>(towlower(c));
        return s;
    }

    // A path for the log: the user's profile folder as %USERPROFILE%.
    std::string Shown(const std::wstring& path)
    {
        wchar_t prof[MAX_PATH];
        const DWORD n = GetEnvironmentVariableW(L"USERPROFILE", prof, MAX_PATH);
        if (n && n < MAX_PATH && Lower(path).compare(0, n, Lower(prof)) == 0)
            return "%USERPROFILE%" + Narrow(path.substr(n));
        return Narrow(path);
    }

    // FNV-1a, 64 bits: not for security, only to tell two files apart.
    struct Fnv
    {
        unsigned long long h = 1469598103934665603ull;
        void Add(const void* p, size_t n)
        {
            const unsigned char* b = static_cast<const unsigned char*>(p);
            for (size_t i = 0; i < n; ++i)
                h = (h ^ b[i]) * 1099511628211ull;
        }
        void Add(const std::string& s) { Add(s.data(), s.size()); }
        unsigned Short() const { return static_cast<unsigned>(h ^ (h >> 32)); }
    };

    bool HashFile(const std::wstring& path, unsigned long long& hash, unsigned long long& size)
    {
        HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (f == INVALID_HANDLE_VALUE)
            return false;
        Fnv fnv;
        std::vector<unsigned char> buf(1 << 16);
        size = 0;
        DWORD got = 0;
        while (ReadFile(f, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr) && got)
        {
            fnv.Add(buf.data(), got);
            size += got;
        }
        CloseHandle(f);
        hash = fnv.h;
        return true;
    }

    std::string FileVersion(const std::wstring& path)
    {
        DWORD ignore = 0;
        const DWORD n = GetFileVersionInfoSizeW(path.c_str(), &ignore);
        if (!n)
            return {};
        std::vector<unsigned char> data(n);
        VS_FIXEDFILEINFO* fi = nullptr;
        UINT len = 0;
        if (!GetFileVersionInfoW(path.c_str(), 0, n, data.data()) ||
            !VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&fi), &len) || !fi)
            return {};
        char v[48];
        snprintf(v, sizeof(v), "%u.%u.%u.%u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS),
                 HIWORD(fi->dwFileVersionLS), LOWORD(fi->dwFileVersionLS));
        return v;
    }

    // "name, 1.2 MB, version 1.2.3.4, hash 0123abcd": what tells two copies of a file apart.
    std::string Describe(const std::wstring& path, unsigned* shortHash = nullptr)
    {
        unsigned long long hash = 0, size = 0;
        if (!HashFile(path, hash, size))
            return "not readable";
        char line[160];
        const std::string ver = FileVersion(path);
        snprintf(line, sizeof(line), "%.0f KB%s%s, hash %016llx", size / 1024.0, ver.empty() ? "" : ", version ",
                 ver.c_str(), hash);
        if (shortHash)
            *shortHash = static_cast<unsigned>(hash ^ (hash >> 32));
        return line;
    }

    // The file's lines, up to 4 MB of it. Opened with every share flag: DXVK holds WoW_d3d9.log open for writing
    // in a way fopen cannot share, and an fopen of it read nothing.
    std::vector<std::string> ReadLines(const std::wstring& path)
    {
        std::vector<std::string> out;
        HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (f == INVALID_HANDLE_VALUE)
            return out;
        std::string text;
        std::vector<char> buf(1 << 16);
        DWORD got = 0;
        while (text.size() < (4u << 20) && ReadFile(f, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr) && got)
            text.append(buf.data(), got);
        CloseHandle(f);
        size_t at = 0;
        while (at < text.size())
        {
            size_t end = text.find('\n', at);
            if (end == std::string::npos)
                end = text.size();
            std::string s = text.substr(at, end - at);
            while (!s.empty() && (s.back() == '\r' || s.back() == ' ' || s.back() == '\t'))
                s.pop_back();
            out.push_back(s);
            at = end + 1;
        }
        return out;
    }

    std::wstring ClientDir()
    {
        wchar_t exe[MAX_PATH];
        const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
        if (!n || n >= MAX_PATH)
            return {};
        std::wstring d(exe);
        return d.substr(0, d.find_last_of(L"\\/") + 1);
    }

    void System()
    {
        typedef LONG(WINAPI * RtlGetVersionFn)(OSVERSIONINFOW*);
        HMODULE nt = GetModuleHandleW(L"ntdll.dll");
        OSVERSIONINFOW os = { sizeof(os) };
        if (auto get = nt ? reinterpret_cast<RtlGetVersionFn>(GetProcAddress(nt, "RtlGetVersion")) : nullptr)
            get(&os);
        typedef const char*(CDECL * WineVersionFn)();
        auto wine = nt ? reinterpret_cast<WineVersionFn>(GetProcAddress(nt, "wine_get_version")) : nullptr;
        BOOL wow64 = FALSE;
        IsWow64Process(GetCurrentProcess(), &wow64);
        Put("system: Windows %lu.%lu build %lu%s%s%s", os.dwMajorVersion, os.dwMinorVersion, os.dwBuildNumber,
            wow64 ? ", 64-bit" : ", 32-bit", wine ? ", under Wine " : "", wine ? wine() : "");

        wchar_t cpu[128] = L"";
        DWORD bytes = sizeof(cpu);
        RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString",
                     RRF_RT_REG_SZ, nullptr, cpu, &bytes);
        SYSTEM_INFO si = {};
        GetNativeSystemInfo(&si);
        MEMORYSTATUSEX mem = { sizeof(mem) };
        GlobalMemoryStatusEx(&mem);
        std::string cpuName = Narrow(cpu);
        while (!cpuName.empty() && cpuName.back() == ' ')
            cpuName.pop_back();
        Put("system: %s, %lu threads, %.1f GB memory", cpuName.empty() ? "CPU not named" : cpuName.c_str(),
            si.dwNumberOfProcessors, mem.ullTotalPhys / (1024.0 * 1024.0 * 1024.0));
    }

    // The process that started WoW.exe: VanillaFixes.exe, a launcher, or the game started by hand.
    std::string Parent()
    {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE)
            return "not known";
        PROCESSENTRY32W pe = { sizeof(pe) };
        DWORD parent = 0;
        const DWORD self = GetCurrentProcessId();
        for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
            if (pe.th32ProcessID == self)
                parent = pe.th32ParentProcessID;
        std::string name = "gone (it exited after starting the game)";
        pe.dwSize = sizeof(pe);
        for (BOOL ok = Process32FirstW(snap, &pe); ok && parent; ok = Process32NextW(snap, &pe))
            if (pe.th32ProcessID == parent)
                name = Narrow(pe.szExeFile);
        CloseHandle(snap);
        return name;
    }

    // Every module in the process. Those from outside the Windows folder are listed with a hash: the client's
    // own DLLs, VanillaFixes' and the mods it loads, DXVK, and overlays that inject themselves.
    void Modules(Fnv& code)
    {
        wchar_t win[MAX_PATH];
        const UINT wn = GetWindowsDirectoryW(win, MAX_PATH);
        const std::wstring winDir = Lower(std::wstring(win, wn)) + L"\\";
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, 0);
        if (snap == INVALID_HANDLE_VALUE)
        {
            Put("modules: could not list them (error %lu)", GetLastError());
            return;
        }
        struct Mod { std::wstring path; std::string name; };
        std::vector<Mod> mine;
        std::vector<std::string> windows;
        MODULEENTRY32W me = { sizeof(me) };
        for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me))
        {
            const std::wstring path = me.szExePath;
            if (Lower(path).compare(0, winDir.size(), winDir) == 0)
                windows.push_back(Narrow(me.szModule));
            else
                mine.push_back({ path, Narrow(me.szModule) });
        }
        CloseHandle(snap);
        std::sort(mine.begin(), mine.end(), [](const Mod& a, const Mod& b) { return Lower(std::wstring(a.path)) < Lower(std::wstring(b.path)); });
        Put("modules: %u from outside the Windows folder, %u from it", static_cast<unsigned>(mine.size()),
            static_cast<unsigned>(windows.size()));
        for (const Mod& m : mine)
        {
            unsigned h = 0;
            const std::string d = Describe(m.path, &h);
            Put("  %s: %s", Shown(m.path).c_str(), d.c_str());
            std::string lower = m.name;
            for (char& c : lower)
                c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
            code.Add(lower);
            code.Add(&h, sizeof(h));
        }
        // From the Windows folder, the ones that draw: a d3d9.dll from there means no DXVK.
        std::string draw;
        for (const std::string& n : windows)
        {
            std::string l = n;
            for (char& c : l)
                c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
            if (l == "d3d9.dll" || l == "d3d11.dll" || l == "dxgi.dll" || l == "opengl32.dll" || l == "vulkan-1.dll" ||
                l == "d3dcompiler_47.dll" || l.compare(0, 2, "nv") == 0 || l.compare(0, 3, "amd") == 0 ||
                l.compare(0, 3, "ati") == 0 || l.compare(0, 2, "ig") == 0)
                draw += (draw.empty() ? "" : ", ") + n;
        }
        Put("modules: from the Windows folder, for drawing: %s", draw.empty() ? "none" : draw.c_str());
    }

    void TextFile(const char* label, const std::wstring& path, bool skipComments, bool skipAccount)
    {
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            Put("%s: not there", label);
            return;
        }
        const std::vector<std::string> lines = ReadLines(path);
        unsigned shown = 0;
        Put("%s:", label);
        for (const std::string& l : lines)
        {
            const size_t first = l.find_first_not_of(" \t");
            if (first == std::string::npos)
                continue;
            if (skipComments && (l[first] == '#' || l[first] == ';'))
                continue;
            std::string lower = l;
            for (char& c : lower)
                c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
            if (skipAccount && lower.find("account") != std::string::npos)
                continue;
            Put("  %s", l.c_str());
            if (++shown >= 200)
            {
                Put("  (the rest left out)");
                break;
            }
        }
    }

    // DXVK's own log of this run (WoW_d3d9.log): its version, the graphics card and driver it found (the D3D9
    // adapter's driver version is a placeholder under DXVK), the configuration in effect, and its errors.
    void Dxvk(const std::wstring& dir)
    {
        const std::wstring path = dir + L"WoW_d3d9.log";
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            Put("dxvk: no WoW_d3d9.log (no DXVK, or its log is off)");
            return;
        }
        std::vector<std::string> seen;
        auto once = [&](const std::string& l) {
            if (std::find(seen.begin(), seen.end(), l) == seen.end())
            {
                seen.push_back(l);
                Put("  %s", l.c_str());
            }
        };
        const std::vector<std::string> lines = ReadLines(path);
        if (lines.empty())
        {
            Put("dxvk: WoW_d3d9.log is empty or could not be read");
            return;
        }
        unsigned errs = 0, warns = 0, errShown = 0;
        bool inConfig = false;
        Put("dxvk (WoW_d3d9.log):");
        for (const std::string& raw : lines)
        {
            const bool info = raw.compare(0, 6, "info: ") == 0;
            const size_t textAt = raw.find_first_not_of(' ', 6);
            const std::string l = info && textAt != std::string::npos ? raw.substr(textAt) : raw;
            if (raw.compare(0, 4, "err:") == 0)
            {
                ++errs;
                if (errShown++ < 5)
                    Put("  %s", raw.c_str());
                continue;
            }
            if (raw.compare(0, 5, "warn:") == 0)
                ++warns;
            if (!info)
                continue;
            if (inConfig && raw.compare(0, 8, "info:   ") == 0)
            {
                once("config: " + l);
                continue;
            }
            inConfig = l.compare(0, 23, "Effective configuration") == 0;
            if (l.compare(0, 5, "DXVK:") == 0 || l.compare(0, 6, "Build:") == 0 || l.compare(0, 13, "Found device:") == 0)
                once(l);
        }
        Put("  %u errors, %u warnings in all", errs, warns);
    }

    // The addons in Interface\AddOns, with the version their .toc gives, several to a line.
    void AddOns(const std::wstring& dir)
    {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"Interface\\AddOns\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE)
        {
            Put("addons: no Interface\\AddOns folder");
            return;
        }
        std::vector<std::string> names;
        do
        {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.')
                continue;
            std::string entry = Narrow(fd.cFileName);
            const std::wstring toc = dir + L"Interface\\AddOns\\" + fd.cFileName + L"\\" + fd.cFileName + L".toc";
            for (const std::string& l : ReadLines(toc))
                if (l.compare(0, 11, "## Version:") == 0)
                {
                    const size_t v = l.find_first_not_of(" \t", 11);
                    if (v != std::string::npos)
                        entry += " " + l.substr(v);
                    break;
                }
            names.push_back(entry);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        Put("addons: %u folders in Interface\\AddOns", static_cast<unsigned>(names.size()));
        std::string line;
        for (const std::string& n : names)
        {
            if (line.size() + n.size() > 110)
            {
                Put("  %s", line.c_str());
                line.clear();
            }
            line += (line.empty() ? "" : ", ") + n;
        }
        if (!line.empty())
            Put("  %s", line.c_str());
    }

    // The MPQs in Data and in its subfolders (the locale's), with size and date.
    void Archives(const std::wstring& dir, Fnv& code)
    {
        std::vector<std::wstring> folders = { L"" };
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"Data\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE)
        {
            do
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.')
                    folders.push_back(std::wstring(fd.cFileName) + L"\\");
            while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        struct Mpq { std::string name; unsigned long long size; SYSTEMTIME at; };
        std::vector<Mpq> all;
        for (const std::wstring& sub : folders)
        {
            h = FindFirstFileW((dir + L"Data\\" + sub + L"*.mpq").c_str(), &fd);
            if (h == INVALID_HANDLE_VALUE)
                continue;
            do
            {
                std::wstring lower = Lower(fd.cFileName);
                if (lower.size() < 4 || lower.compare(lower.size() - 4, 4, L".mpq") != 0)
                    continue;   // "*.mpq" also matches "x.mpq.off" through its short name
                Mpq m;
                m.name = Narrow(sub + fd.cFileName);
                m.size = (static_cast<unsigned long long>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
                FileTimeToSystemTime(&fd.ftLastWriteTime, &m.at);
                all.push_back(m);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        std::sort(all.begin(), all.end(), [](const Mpq& a, const Mpq& b) { return a.name < b.name; });
        Put("data: %u MPQs in Data and its folders", static_cast<unsigned>(all.size()));
        for (const Mpq& m : all)
        {
            Put("  %s: %.1f MB, %04u-%02u-%02u", m.name.c_str(), m.size / (1024.0 * 1024.0), m.at.wYear, m.at.wMonth,
                m.at.wDay);
            std::string lower = m.name;
            for (char& c : lower)
                c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
            code.Add(lower);
            code.Add(&m.size, sizeof(m.size));
        }
    }

    struct Job
    {
        std::string why, settings, defaults, tuned, controls, device;
        unsigned    settingsCode;
    };

    DWORD WINAPI Run(void* p)
    {
        Job* j = static_cast<Job*>(p);
        std::string out;
        t_out = &out;
        const double t0 = Now();
        const std::wstring dir = ClientDir();
        Put("=== client report (%s) ===", j->why.c_str());

        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&Run), &self);
        wchar_t selfPath[MAX_PATH] = L"";
        GetModuleFileNameW(self, selfPath, MAX_PATH);
        Put("comfyfog: %s, built %s %s, %s", COMFYFOG_VERSION, __DATE__, __TIME__, Describe(selfPath).c_str());
        Put("client folder: %s", Shown(dir).c_str());
        unsigned exeCode = 0;
        Put("WoW.exe: %s", Describe(dir + L"WoW.exe", &exeCode).c_str());
        Put("started by: %s", Parent().c_str());
        Put("VanillaFixes.exe: %s", Describe(dir + L"VanillaFixes.exe").c_str());
        System();
        Put("graphics: %s", j->device.empty() ? "no device yet" : j->device.c_str());
        Fnv dlls, data;
        Modules(dlls);
        TextFile("dlls.txt (what VanillaFixes loads)", dir + L"dlls.txt", true, false);
        TextFile("dxvk.conf", dir + L"dxvk.conf", true, false);
        Dxvk(dir);
        TextFile("WTF\\Config.wtf (the account lines left out)", dir + L"WTF\\Config.wtf", false, true);
        AddOns(dir);
        Archives(dir, data);
        Put("settings (comfyfog.ini, with /atmos on top):");
        size_t at = 0;
        while (at < j->settings.size())
        {
            const size_t end = j->settings.find('\n', at);
            Put("  %s", j->settings.substr(at, end - at).c_str());
            at = end == std::string::npos ? j->settings.size() : end + 1;
        }
        Put("settings not in comfyfog.ini (the built-in values): %s", j->defaults.empty() ? "none" : j->defaults.c_str());
        Put("settings set by /atmos: %s", j->tuned.empty() ? "none" : j->tuned.c_str());
        Put("controls (Video > Atmosphere): %s", j->controls.c_str());
        Fnv ctl;
        ctl.Add(j->controls);
        Put("fingerprint: WoW.exe %08x, DLLs %08x, data %08x, settings %08x, controls %08x", exeCode, dlls.Short(),
            data.Short(), j->settingsCode, ctl.Short());
        Put("=== end of client report (%.0f ms) ===", 1000.0 * (Now() - t0));
        t_out = nullptr;
        if (!out.empty() && out.back() == '\n')
            out.pop_back();
        Log("%s", out.c_str());
        delete j;
        return 0;
    }
}

void ReportDevice(IDirect3DDevice9* dev)
{
    std::string text;
    IDirect3D9* d3d = nullptr;
    D3DDEVICE_CREATION_PARAMETERS cp = {};
    if (SUCCEEDED(dev->lpVtbl->GetCreationParameters(dev, &cp)) && SUCCEEDED(dev->lpVtbl->GetDirect3D(dev, &d3d)) && d3d)
    {
        D3DADAPTER_IDENTIFIER9 id = {};
        if (SUCCEEDED(d3d->lpVtbl->GetAdapterIdentifier(d3d, cp.AdapterOrdinal, 0, &id)))
        {
            char line[400];
            snprintf(line, sizeof(line), "%s (vendor %04lx, device %04lx), driver %s %u.%u.%u.%u", id.Description,
                     id.VendorId, id.DeviceId, id.Driver, HIWORD(id.DriverVersion.HighPart),
                     LOWORD(id.DriverVersion.HighPart), HIWORD(id.DriverVersion.LowPart),
                     LOWORD(id.DriverVersion.LowPart));
            text = line;
        }
        d3d->lpVtbl->Release(d3d);
    }
    IDirect3DSwapChain9* sc = nullptr;
    if (SUCCEEDED(dev->lpVtbl->GetSwapChain(dev, 0, &sc)) && sc)
    {
        D3DPRESENT_PARAMETERS pp = {};
        if (SUCCEEDED(sc->lpVtbl->GetPresentParameters(sc, &pp)))
        {
            char line[200];
            snprintf(line, sizeof(line), "; back buffer %ux%u, format %d, %s, multisample %d, refresh %u Hz, vsync %s",
                     pp.BackBufferWidth, pp.BackBufferHeight, pp.BackBufferFormat, pp.Windowed ? "windowed" : "full screen",
                     pp.MultiSampleType, pp.FullScreen_RefreshRateInHz,
                     pp.PresentationInterval == D3DPRESENT_INTERVAL_IMMEDIATE ? "off" : "on");
            text += line;
        }
        sc->lpVtbl->Release(sc);
    }
    bool first = false;
    {
        std::lock_guard<std::mutex> lock(g_mx);
        g_device = text;
        first = !g_started;
        g_started = true;
    }
    Log("graphics: %s", text.c_str());
    if (first)
        ReportStart("at start");
}

void ReportStart(const char* why)
{
    Job* j = new Job();
    j->why = why;
    // The settings, by section, one line each.
    std::string section;
    for (const ConfigKey& k : ConfigKeys())
    {
        if (k.section != section)
        {
            section = k.section;
            j->settings += (j->settings.empty() ? "" : "\n") + ("[" + section + "]");
        }
        j->settings += " " + k.key + "=" + k.value;
        if (k.source == kFromDefault)
            j->defaults += (j->defaults.empty() ? "" : ", ") + k.section + "." + k.key;
        else if (k.source == kFromTune)
            j->tuned += (j->tuned.empty() ? "" : ", ") + k.section + "." + k.key + "=" + k.value;
    }
    Fnv s;
    s.Add(j->settings);
    j->settingsCode = s.Short();
    CVarsControlsText(j->controls);
    {
        std::lock_guard<std::mutex> lock(g_mx);
        j->device = g_device;
    }
    HANDLE t = CreateThread(nullptr, 0, Run, j, 0, nullptr);
    if (t)
        CloseHandle(t);
    else
    {
        Log("client report: could not start its thread");
        delete j;
    }
}
