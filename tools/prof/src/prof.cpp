// comfyprof.dll (2026-10-20): a sampling profiler for the 1.12 client. Test clients only.
//
// Write "<seconds> <label>" into <client>\Imports\comfyprof-go.txt. The DLL takes the file away, samples for that
// long and appends a report to <client>\Logs\comfyprof.log:
//
// - every thread's CPU over the window (QueryThreadCycleTime), by its name where DXVK gave it one;
// - the game's thread (the one that owns the game's window), sampled about 1,000 times a second, and DXVK's CS
//   thread the same way: which module each sample is in, and for a sample in Windows or the driver, which of
//   our modules called it, and whether it waits there (an NtWait or NtDelay system call);
// - the functions most often on the game thread's stack (inclusive), and the ones most often at the top (self).
//
// A thread is suspended for each sample only long enough to read its registers and copy its stack; nothing is
// allocated while it is suspended, since it may hold the heap's lock. The stack is then searched for return
// addresses: a value that points just after a CALL instruction in a module's code. A direct call (E8) also
// names the function it called, which is how functions get an address without symbols.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <mmsystem.h>
#include <dbghelp.h>

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <share.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    wchar_t g_clientDir[MAX_PATH] = {};
    wchar_t g_logPath[MAX_PATH]   = {};
    wchar_t g_goPath[MAX_PATH]    = {};

    void Log(const char* fmt, ...)
    {
        FILE* f = _wfsopen(g_logPath, L"a", _SH_DENYNO);
        if (!f)
            return;
        va_list ap;
        va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);
        fputc('\n', f);
        fclose(f);
    }

    double Now()
    {
        static LARGE_INTEGER f = [] { LARGE_INTEGER q; QueryPerformanceFrequency(&q); return q; }();
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        return static_cast<double>(t.QuadPart) / static_cast<double>(f.QuadPart);
    }

    // ------------------------------------------------------------------------------------------------ modules

    struct Range { uintptr_t lo, hi; };

    struct Module
    {
        uintptr_t          base = 0, end = 0;
        std::string        name;
        bool               system = false;   // under the Windows folder: Windows itself and the driver
        std::vector<Range> code;             // executable sections
        std::vector<std::pair<uintptr_t, std::string>> exports;   // sorted, for system modules
    };

    std::vector<Module> g_mods;   // sorted by base

    void ReadExports(Module& m)
    {
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(m.base);
        auto* nt  = reinterpret_cast<IMAGE_NT_HEADERS32*>(m.base + dos->e_lfanew);
        const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (!dir.VirtualAddress || !dir.Size)
            return;
        auto* ex    = reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(m.base + dir.VirtualAddress);
        auto* names = reinterpret_cast<DWORD*>(m.base + ex->AddressOfNames);
        auto* ords  = reinterpret_cast<WORD*>(m.base + ex->AddressOfNameOrdinals);
        auto* funcs = reinterpret_cast<DWORD*>(m.base + ex->AddressOfFunctions);
        for (DWORD i = 0; i < ex->NumberOfNames; ++i)
        {
            const DWORD rva = funcs[ords[i]];
            if (rva >= dir.VirtualAddress && rva < dir.VirtualAddress + dir.Size)
                continue;   // a forwarder
            m.exports.emplace_back(m.base + rva, reinterpret_cast<const char*>(m.base + names[i]));
        }
        std::sort(m.exports.begin(), m.exports.end());
    }

    void SnapshotModules()
    {
        g_mods.clear();
        wchar_t winDir[MAX_PATH] = {};
        GetWindowsDirectoryW(winDir, MAX_PATH);
        const size_t winLen = wcslen(winDir);
        HMODULE hm[1024];
        DWORD   need = 0;
        if (!EnumProcessModules(GetCurrentProcess(), hm, sizeof(hm), &need))
            return;
        const DWORD n = std::min<DWORD>(need / sizeof(HMODULE), 1024);
        for (DWORD i = 0; i < n; ++i)
        {
            MODULEINFO mi = {};
            if (!GetModuleInformation(GetCurrentProcess(), hm[i], &mi, sizeof(mi)))
                continue;
            Module m;
            m.base = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll);
            m.end  = m.base + mi.SizeOfImage;
            wchar_t path[MAX_PATH] = {};
            GetModuleFileNameW(hm[i], path, MAX_PATH);
            m.system = _wcsnicmp(path, winDir, winLen) == 0;
            const wchar_t* slash = wcsrchr(path, L'\\');
            char nm[MAX_PATH];
            WideCharToMultiByte(CP_UTF8, 0, slash ? slash + 1 : path, -1, nm, MAX_PATH, nullptr, nullptr);
            m.name = nm;
            auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(m.base);
            auto* nt  = reinterpret_cast<IMAGE_NT_HEADERS32*>(m.base + dos->e_lfanew);
            auto* sec = IMAGE_FIRST_SECTION(nt);
            for (WORD s = 0; s < nt->FileHeader.NumberOfSections; ++s)
                if (sec[s].Characteristics & IMAGE_SCN_MEM_EXECUTE)
                    m.code.push_back({ m.base + sec[s].VirtualAddress,
                                       m.base + sec[s].VirtualAddress + std::max(sec[s].Misc.VirtualSize, sec[s].SizeOfRawData) });
            ReadExports(m);
            g_mods.push_back(std::move(m));
        }
        std::sort(g_mods.begin(), g_mods.end(), [](const Module& a, const Module& b) { return a.base < b.base; });
    }

    int ModuleOf(uintptr_t a)
    {
        auto it = std::upper_bound(g_mods.begin(), g_mods.end(), a, [](uintptr_t v, const Module& m) { return v < m.base; });
        if (it == g_mods.begin())
            return -1;
        --it;
        return a < it->end ? static_cast<int>(it - g_mods.begin()) : -1;
    }

    bool InCode(const Module& m, uintptr_t a, uintptr_t before)
    {
        for (const Range& r : m.code)
            if (a >= r.lo + before && a < r.hi)
                return true;
        return false;
    }

    const char* ExportName(const Module& m, uintptr_t a, uintptr_t* off)
    {
        auto it = std::upper_bound(m.exports.begin(), m.exports.end(), a,
                                   [](uintptr_t v, const std::pair<uintptr_t, std::string>& e) { return v < e.first; });
        if (it == m.exports.begin())
            return nullptr;
        --it;
        *off = a - it->first;
        return it->second.c_str();
    }

    // A return address: r points just after a CALL in a module's code. *callee is the target of a direct call.
    bool IsReturn(uintptr_t r, int* mod, uintptr_t* callee)
    {
        const int mi = ModuleOf(r);
        if (mi < 0 || !InCode(g_mods[mi], r, 7))
            return false;
        const auto* b = reinterpret_cast<const uint8_t*>(r);
        *mod    = mi;
        *callee = 0;
        if (b[-5] == 0xE8)
        {
            const uintptr_t t = r + *reinterpret_cast<const int32_t*>(r - 4);
            const int tm = ModuleOf(t);
            if (tm < 0 || !InCode(g_mods[tm], t, 0))
                return false;
            *callee = t;
            return true;
        }
        auto callModrm = [](uint8_t m) { return (m & 0x38) == 0x10; };
        if (b[-6] == 0xFF && callModrm(b[-5]) && ((b[-5] & 0xC7) == 0x05 || ((b[-5] & 0xC0) == 0x80 && (b[-5] & 7) != 4)))
            return true;   // call [disp32], call [reg+disp32]
        if (b[-7] == 0xFF && callModrm(b[-6]) && (b[-6] & 0xC0) == 0x80 && (b[-6] & 7) == 4)
            return true;   // call [sib+disp32]
        if (b[-3] == 0xFF && callModrm(b[-2]) && (b[-2] & 0xC0) == 0x40 && (b[-2] & 7) != 4)
            return true;   // call [reg+disp8]
        if (b[-4] == 0xFF && callModrm(b[-3]) && (b[-3] & 0xC0) == 0x40 && (b[-3] & 7) == 4)
            return true;   // call [sib+disp8]
        if (b[-2] == 0xFF && callModrm(b[-1]) && ((b[-1] & 0xC0) == 0xC0 || ((b[-1] & 0xC0) == 0 && (b[-1] & 7) != 4 && (b[-1] & 7) != 5)))
            return true;   // call reg, call [reg]
        if (b[-3] == 0xFF && callModrm(b[-2]) && (b[-2] & 0xC0) == 0 && (b[-2] & 7) == 4)
            return true;   // call [sib]
        return false;
    }

    // ------------------------------------------------------------------------------------------------ threads

    using NtQueryInformationThreadFn = LONG(NTAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
    struct ThreadBasic { LONG exitStatus; PVOID teb; PVOID pid; PVOID tid; ULONG_PTR affinity; LONG prio, basePrio; };

    struct Thread
    {
        DWORD     tid = 0;
        HANDLE    h = nullptr;
        std::string name;
        ULONG64   cyc0 = 0, cyc1 = 0;
        uintptr_t stackBase = 0;
    };

    std::string ThreadName(HANDLE h)
    {
        using GetDescFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
        static auto fn = reinterpret_cast<GetDescFn>(GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "GetThreadDescription"));
        if (!fn)
            return {};
        PWSTR d = nullptr;
        std::string out;
        if (SUCCEEDED(fn(h, &d)) && d)
        {
            char nm[256];
            WideCharToMultiByte(CP_UTF8, 0, d, -1, nm, sizeof(nm), nullptr, nullptr);
            out = nm;
            LocalFree(d);
        }
        return out;
    }

    uintptr_t StackBase(HANDLE h)
    {
        static auto q = reinterpret_cast<NtQueryInformationThreadFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
        ThreadBasic tb = {};
        if (!q || q(h, 0, &tb, sizeof(tb), nullptr) != 0 || !tb.teb)
            return 0;
        return reinterpret_cast<uintptr_t>(reinterpret_cast<NT_TIB*>(tb.teb)->StackBase);
    }

    DWORD g_mainTid = 0;
    BOOL CALLBACK FindWindowProc(HWND w, LPARAM)
    {
        DWORD pid = 0;
        const DWORD tid = GetWindowThreadProcessId(w, &pid);
        if (pid != GetCurrentProcessId() || !IsWindowVisible(w))
            return TRUE;
        char cls[128] = {};
        GetClassNameA(w, cls, sizeof(cls));
        if (strstr(cls, "GxWindowClass"))
        {
            g_mainTid = tid;
            return FALSE;
        }
        return TRUE;
    }

    // ------------------------------------------------------------------------------------------------ sampling

    constexpr size_t kStackCopy = 16384;

    struct Tally
    {
        long long samples = 0;
        std::map<std::string, long long> where;          // "module" or "module > system (call)"
        std::unordered_map<int, long long> leafMod;
        std::unordered_map<int, long long> inclMod;
        std::unordered_map<uintptr_t, long long> inclFn;  // direct-call targets on the stack
        std::unordered_map<uintptr_t, long long> self;    // the innermost function (direct-call target) or 256-byte bucket
        std::unordered_map<uintptr_t, long long> d3dSite; // the game's call sites into d3d9.dll (return addresses)
        std::unordered_map<uintptr_t, long long> leafEip; // every sample's instruction, for the symbols at the report
        std::unordered_map<uintptr_t, long long> chain;   // return addresses on the EBP chain (once a sample)
    };

    int g_d3d9 = -1, g_wow = -1;

    bool IsWait(const char* sys)
    {
        return sys && (strstr(sys, "Wait") || strstr(sys, "Delay") || strstr(sys, "RemoveIo") || strstr(sys, "Yield") ||
                       strstr(sys, "Sleep"));
    }

    void Analyse(Tally& t, uintptr_t eip, uintptr_t esp, uintptr_t ebp, const uint32_t* stack, size_t words)
    {
        ++t.samples;
        t.leafEip[eip]++;
        {
            // The EBP chain: exact where every frame keeps a frame pointer (comfyatmos on the perf branch: /Oy-).
            std::unordered_set<uintptr_t> seen;
            uintptr_t e = ebp;
            for (int k = 0; k < 64 && e >= esp && e + 8 <= esp + words * 4; ++k)
            {
                const size_t i = (e - esp) / 4;
                const uintptr_t ret = stack[i + 1], next = stack[i];
                int m; uintptr_t callee;
                if (!IsReturn(ret, &m, &callee))
                    break;
                if (seen.insert(ret).second)
                    t.chain[ret]++;
                if (next <= e)
                    break;
                e = next;
            }
        }
        const int lm = ModuleOf(eip);
        t.leafMod[lm]++;
        std::unordered_set<int> mods;
        std::unordered_set<uintptr_t> fns;
        mods.insert(lm);
        int       firstOwn = -1;           // the first non-system module on the stack (the leaf counts)
        uintptr_t innerCallee = 0;         // the innermost direct-call target, for the self table
        int       prevMod = lm;
        bool      selfSet = false;
        if (lm >= 0 && !g_mods[lm].system)
            firstOwn = lm;
        int found = 0;
        for (size_t i = 0; i < words && found < 96; ++i)
        {
            int m; uintptr_t callee;
            if (!IsReturn(stack[i], &m, &callee))
                continue;
            ++found;
            mods.insert(m);
            if (firstOwn < 0 && !g_mods[m].system)
                firstOwn = m;
            if (callee)
            {
                fns.insert(callee);
                if (!selfSet && lm >= 0 && ModuleOf(callee) == lm && callee <= eip)
                {
                    innerCallee = callee;
                    selfSet = true;
                }
            }
            if (m == g_wow && prevMod == g_d3d9)
                t.d3dSite[stack[i]]++;
            prevMod = m;
        }
        for (int m : mods)
            t.inclMod[m]++;
        for (uintptr_t f : fns)
            t.inclFn[f]++;
        t.self[selfSet ? innerCallee : (eip & ~uintptr_t(0xFF))]++;

        std::string w = firstOwn >= 0 ? g_mods[firstOwn].name : std::string("?");
        if (lm >= 0 && g_mods[lm].system)
        {
            uintptr_t off = 0;
            const char* sys = ExportName(g_mods[lm], eip, &off);
            if (IsWait(sys))
                w += std::string(" WAIT ") + sys;
            else
                w += " > " + g_mods[lm].name;
        }
        else if (lm < 0)
            w += " > (no module)";
        t.where[w]++;
    }

    struct Sampled
    {
        Thread*   th = nullptr;
        Tally     tally;
        uint32_t* buf = nullptr;
    };

    bool SampleOnce(Sampled& s)
    {
        if (SuspendThread(s.th->h) == static_cast<DWORD>(-1))
            return false;
        CONTEXT c = {};
        c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        size_t words = 0;
        uintptr_t eip = 0, sp = 0, bp = 0;
        if (GetThreadContext(s.th->h, &c))
        {
            eip = c.Eip;
            sp  = c.Esp;
            bp  = c.Ebp;
            if (s.th->stackBase > sp)
            {
                const size_t bytes = std::min<size_t>(kStackCopy, s.th->stackBase - sp);
                memcpy(s.buf, reinterpret_cast<const void*>(sp), bytes);
                words = bytes / 4;
            }
        }
        ResumeThread(s.th->h);
        if (!eip)
            return false;
        Analyse(s.tally, eip, sp, bp, s.buf, words);
        return true;
    }

    // Names for WoW.exe 1.12.1 (build 5875) functions, "ADDR NAME" a line: comfyprof-names.txt beside WoW.exe,
    // made from wow_turbo's symbols.toml (github.com/athei/wow-mods). Turtle's exe may differ in places.
    std::map<uintptr_t, std::string> g_wowNames;

    void LoadWowNames()
    {
        if (!g_wowNames.empty())
            return;
        wchar_t path[MAX_PATH];
        swprintf(path, MAX_PATH, L"%s\\comfyprof-names.txt", g_clientDir);
        FILE* f = _wfsopen(path, L"r", _SH_DENYNO);
        if (!f)
            return;
        char line[256];
        while (fgets(line, sizeof(line), f))
        {
            char nm[200] = {};
            unsigned a = 0;
            if (sscanf_s(line, "%x %199s", &a, nm, static_cast<unsigned>(sizeof(nm))) == 2)
                g_wowNames[a] = nm;
        }
        fclose(f);
    }

    std::string FnName(uintptr_t a, bool sym)
    {
        char buf[512];
        if (auto it = g_wowNames.find(a); it != g_wowNames.end())
        {
            snprintf(buf, sizeof(buf), "WoW.exe!%s (%08X)", it->second.c_str(), static_cast<unsigned>(a));
            return buf;
        }
        const int mi = ModuleOf(a);
        if (mi < 0)
        {
            snprintf(buf, sizeof(buf), "%08X", static_cast<unsigned>(a));
            return buf;
        }
        const Module& m = g_mods[mi];
        if (sym)
        {
            alignas(SYMBOL_INFO) char sb[sizeof(SYMBOL_INFO) + 256] = {};
            auto* si = reinterpret_cast<SYMBOL_INFO*>(sb);
            si->SizeOfStruct = sizeof(SYMBOL_INFO);
            si->MaxNameLen   = 255;
            DWORD64 disp = 0;
            if (SymFromAddr(GetCurrentProcess(), a, &disp, si))
            {
                snprintf(buf, sizeof(buf), "%s!%s+%X (%08X)", m.name.c_str(), si->Name, static_cast<unsigned>(disp), static_cast<unsigned>(a));
                return buf;
            }
        }
        uintptr_t off = 0;
        if (const char* e = ExportName(m, a, &off); e && off < 0x10000)
        {
            snprintf(buf, sizeof(buf), "%s!%s+%X (%08X)", m.name.c_str(), e, static_cast<unsigned>(off), static_cast<unsigned>(a));
            return buf;
        }
        snprintf(buf, sizeof(buf), "%s+%X (%08X)", m.name.c_str(), static_cast<unsigned>(a - m.base), static_cast<unsigned>(a));
        return buf;
    }

    template <typename K>
    std::vector<std::pair<K, long long>> Top(const std::unordered_map<K, long long>& m, size_t n)
    {
        std::vector<std::pair<K, long long>> v(m.begin(), m.end());
        std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        if (v.size() > n)
            v.resize(n);
        return v;
    }

    void Report(const char* label, double secs, std::vector<Thread>& threads, std::vector<Sampled>& sampled, bool sym)
    {
        SYSTEMTIME st;
        GetLocalTime(&st);
        Log("=== comfyprof %02d:%02d:%02d \"%s\": %.1f s", st.wHour, st.wMinute, st.wSecond, label, secs);

        // The TSC's rate, for the cycle counts: QueryThreadCycleTime counts in TSC ticks.
        static double tscHz = [] {
            const double t0 = Now(); const unsigned long long c0 = __rdtsc();
            Sleep(50);
            return static_cast<double>(__rdtsc() - c0) / (Now() - t0);
        }();
        std::sort(threads.begin(), threads.end(), [](const Thread& a, const Thread& b) { return a.cyc1 - a.cyc0 > b.cyc1 - b.cyc0; });
        Log("threads by CPU (of one core):");
        double total = 0;
        for (const Thread& t : threads)
            total += static_cast<double>(t.cyc1 - t.cyc0) / tscHz / secs;
        int shown = 0;
        for (const Thread& t : threads)
        {
            const double pct = 100.0 * static_cast<double>(t.cyc1 - t.cyc0) / tscHz / secs;
            if (pct < 0.5 || ++shown > 16)
                break;
            Log("  %5.1f%%  tid %5u %s%s", pct, t.tid, t.name.empty() ? "(no name)" : t.name.c_str(), t.tid == g_mainTid ? "  <- the game's thread" : "");
        }
        Log("  all threads: %.0f%% of one core", 100.0 * total);

        for (Sampled& s : sampled)
        {
            const Tally& t = s.tally;
            if (!t.samples)
                continue;
            const double n = static_cast<double>(t.samples);
            Log("--- thread %u %s: %lld samples", s.th->tid, s.th->tid == g_mainTid ? "(the game's thread)" : s.th->name.c_str(), t.samples);
            Log("where (the first of our modules on the stack; > a system module it is in; WAIT a system call that waits):");
            std::vector<std::pair<std::string, long long>> w(t.where.begin(), t.where.end());
            std::sort(w.begin(), w.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
            for (size_t i = 0; i < w.size() && i < 24; ++i)
                Log("  %5.1f%%  %s", 100.0 * w[i].second / n, w[i].first.c_str());
            Log("modules on the stack (inclusive):");
            for (auto& [m, c] : Top(t.inclMod, 16))
                Log("  %5.1f%%  %s", 100.0 * c / n, m >= 0 ? g_mods[m].name.c_str() : "(no module)");
            Log("functions on the stack (inclusive, direct calls):");
            for (auto& [f, c] : Top(t.inclFn, s.th->tid == g_mainTid ? 70 : 25))
                Log("  %5.1f%%  %s", 100.0 * c / n, FnName(f, sym).c_str());
            Log("self (the innermost function called directly in the leaf's module, else a 256-byte block):");
            for (auto& [f, c] : Top(t.self, s.th->tid == g_mainTid ? 45 : 20))
                Log("  %5.1f%%  %s", 100.0 * c / n, FnName(f, sym).c_str());
            if (sym)
            {
                // Our own code by the pdb: self from the sampled instruction, inclusive from the EBP chain.
                std::unordered_map<std::string, long long> ownSelf, ownIncl, wowSelf;
                auto symOf = [](uintptr_t a, std::string& out) -> bool {
                    const int mi = ModuleOf(a);
                    if (mi < 0 || g_mods[mi].name.rfind("comfyatmos", 0) != 0)
                        return false;
                    alignas(SYMBOL_INFO) char sb[sizeof(SYMBOL_INFO) + 256] = {};
                    auto* si = reinterpret_cast<SYMBOL_INFO*>(sb);
                    si->SizeOfStruct = sizeof(SYMBOL_INFO);
                    si->MaxNameLen   = 255;
                    DWORD64 disp = 0;
                    if (!SymFromAddr(GetCurrentProcess(), a, &disp, si))
                        return false;
                    out = si->Name;
                    return true;
                };
                std::string nm;
                long long ownTotal = 0;
                for (auto& [a, c] : t.leafEip)
                    if (symOf(a, nm))
                        ownSelf[nm] += c, ownTotal += c;
                for (auto& [a, c] : t.chain)
                    if (symOf(a, nm))
                        ownIncl[nm] += c;
                auto top = [](std::unordered_map<std::string, long long>& m, size_t n) {
                    std::vector<std::pair<std::string, long long>> v(m.begin(), m.end());
                    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
                    if (v.size() > n) v.resize(n);
                    return v;
                };
                if (ownTotal)
                {
                    Log("comfyatmos self, by the pdb (%.1f%% of the samples in all):", 100.0 * ownTotal / n);
                    for (auto& [f, c] : top(ownSelf, 40))
                        Log("  %5.1f%%  %s", 100.0 * c / n, f.c_str());
                    Log("comfyatmos inclusive, by the EBP chain (callers):");
                    for (auto& [f, c] : top(ownIncl, 50))
                        Log("  %5.1f%%  %s", 100.0 * c / n, f.c_str());
                }
            }
            if (!t.d3dSite.empty())
            {
                Log("the game's calls into d3d9.dll (return addresses in WoW.exe, time under them):");
                for (auto& [f, c] : Top(t.d3dSite, 30))
                    Log("  %5.1f%%  %08X", 100.0 * c / n, static_cast<unsigned>(f));
            }
        }
        Log("=== end");
    }

    void Profile(double secs, const char* label)
    {
        SnapshotModules();
        g_d3d9 = g_wow = -1;
        for (size_t i = 0; i < g_mods.size(); ++i)
        {
            if (_stricmp(g_mods[i].name.c_str(), "d3d9.dll") == 0) g_d3d9 = static_cast<int>(i);
            if (_stricmp(g_mods[i].name.c_str(), "WoW.exe") == 0)  g_wow  = static_cast<int>(i);
        }
        g_mainTid = 0;
        EnumWindows(FindWindowProc, 0);

        std::vector<Thread> threads;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 te = { sizeof(te) };
        if (snap != INVALID_HANDLE_VALUE && Thread32First(snap, &te))
            do
            {
                if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId())
                    continue;
                Thread t;
                t.tid = te.th32ThreadID;
                t.h   = OpenThread(THREAD_QUERY_INFORMATION | THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, t.tid);
                if (!t.h)
                    continue;
                t.name = ThreadName(t.h);
                t.stackBase = StackBase(t.h);
                threads.push_back(t);
            } while (Thread32Next(snap, &te));
        if (snap != INVALID_HANDLE_VALUE)
            CloseHandle(snap);

        // Sampled: the game's thread and DXVK's CS thread.
        std::vector<Sampled> sampled;
        for (Thread& t : threads)
            if (t.tid == g_mainTid || t.name == "dxvk-cs")
            {
                Sampled s;
                s.th  = &t;
                s.buf = static_cast<uint32_t*>(VirtualAlloc(nullptr, kStackCopy, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
                sampled.push_back(std::move(s));
            }
        for (Sampled& s : sampled)   // reserve the tables' room before any thread is suspended
        {
            s.tally.inclFn.reserve(8192);
            s.tally.self.reserve(8192);
            s.tally.d3dSite.reserve(1024);
        }

        for (Thread& t : threads)
            QueryThreadCycleTime(t.h, &t.cyc0);
        timeBeginPeriod(1);
        const double t0 = Now();
        while (Now() - t0 < secs)
        {
            for (Sampled& s : sampled)
                SampleOnce(s);
            Sleep(1);
        }
        const double t1 = Now();
        timeEndPeriod(1);
        for (Thread& t : threads)
            QueryThreadCycleTime(t.h, &t.cyc1);

        LoadWowNames();
        const bool sym = SymInitialize(GetCurrentProcess(), nullptr, TRUE) != FALSE;
        Report(label, t1 - t0, threads, sampled, sym);
        if (sym)
            SymCleanup(GetCurrentProcess());
        for (Sampled& s : sampled)
            VirtualFree(s.buf, 0, MEM_RELEASE);
        for (Thread& t : threads)
            CloseHandle(t.h);
    }

    struct Job { double secs; char label[128]; };
    volatile LONG g_busy = 0;

    DWORD WINAPI RunJob(LPVOID p)
    {
        Job* j = static_cast<Job*>(p);
        Profile(j->secs, j->label);
        delete j;
        InterlockedExchange(&g_busy, 0);
        return 0;
    }

    DWORD WINAPI Worker(LPVOID)
    {
        for (;;)
        {
            Sleep(200);
            FILE* f = _wfsopen(g_goPath, L"r", _SH_DENYNO);
            if (!f)
                continue;
            char line[256] = {};
            fgets(line, sizeof(line), f);
            fclose(f);
            DeleteFileW(g_goPath);
            double secs = atof(line);
            if (secs <= 0 || secs > 120)
                secs = 10;
            const char* label = strchr(line, ' ');
            std::string lab = label ? label + 1 : "";
            while (!lab.empty() && (lab.back() == '\n' || lab.back() == '\r' || lab.back() == ' '))
                lab.pop_back();
            if (InterlockedCompareExchange(&g_busy, 1, 0) != 0)
                continue;
            Profile(secs, lab.c_str());
            InterlockedExchange(&g_busy, 0);
        }
    }
}

// comfyatmos's /atmos prof (2026-10-20): a profile from inside the game, so a test's steps can start one.
extern "C" __declspec(dllexport) int __cdecl ComfyProfStart(double secs, const char* label)
{
    if (InterlockedCompareExchange(&g_busy, 1, 0) != 0)
        return 0;
    Job* j = new Job{ secs > 0 && secs <= 120 ? secs : 10.0, {} };
    strncpy_s(j->label, label ? label : "", _TRUNCATE);
    CloseHandle(CreateThread(nullptr, 0, RunJob, j, 0, nullptr));
    return 1;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        GetModuleFileNameW(nullptr, g_clientDir, MAX_PATH);
        if (wchar_t* s = wcsrchr(g_clientDir, L'\\'))
            *s = 0;
        swprintf(g_logPath, MAX_PATH, L"%s\\Logs\\comfyprof.log", g_clientDir);
        swprintf(g_goPath, MAX_PATH, L"%s\\Imports\\comfyprof-go.txt", g_clientDir);
        DeleteFileW(g_goPath);
        Log("comfyprof loaded");
        CloseHandle(CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr));
    }
    return TRUE;
}
