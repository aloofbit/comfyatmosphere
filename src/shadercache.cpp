// shadercache.cpp: the shaders' cache and the worker that fills it (2026-10-06).
//
// Measured at sign-in (the owner asked whether the effects could come in one by one): 44 compiles, 11.1 s, all on
// the game's thread, in two frames that froze for 5.2 and 7.0 s: the sun shadows' pixel shader alone 3.5 s, the
// water's 4.1 s, the lamp glow's ten variants 2.3 s. The targets, masks and map files made no slow frame of their
// own. So:
//   - Every compile goes through ShaderCacheCompile, keyed by a 64-bit FNV-1a hash of the source, the name, the
//     entry, the profile, the flags and the defines. A result is kept for the session, and on disk as
//     comfyfog-cache\<key>.cso, so a later start reads it in a millisecond. A changed source is a new key.
//   - A worker started with the DLL compiles (or reads) every shader the passes list, below normal priority, while
//     the client is still at its login screen. A key the game's thread asks for while the worker has it waits for
//     the worker instead of compiling it twice.
//   - A failed compile is kept for the session too (not on disk), with its message: the cover pass tries ps_2_0
//     first and expects it to fail for one shader.
// The terrain's copies are built from the game's own shaders and are not listed; they take a few milliseconds.

#include "shadercache.h"

#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    struct Entry
    {
        enum State { kPending, kReady, kFailed } state = kPending;
        std::vector<unsigned char> code;
        std::string                errors;
        HRESULT                    hr = S_OK;
    };

    struct Job
    {
        std::string name, src, profile;
        std::vector<std::string> defines;   // name, value, name, value ...
    };

    std::mutex                              g_m;
    std::condition_variable                 g_cv;
    std::unordered_map<uint64_t, Entry>     g_entries;
    std::vector<Job>                        g_jobs;
    std::wstring                            g_dir;

    using PFN_D3DCreateBlob = HRESULT(WINAPI*)(SIZE_T, OgBlob**);

    void Mix(uint64_t& h, const void* p, size_t n)
    {
        const unsigned char* b = static_cast<const unsigned char*>(p);
        for (size_t i = 0; i < n; ++i)
        {
            h ^= b[i];
            h *= 1099511628211ull;
        }
        const unsigned char sep = 0xFF;   // so "ab"+"c" and "a"+"bc" differ
        h ^= sep;
        h *= 1099511628211ull;
    }

    uint64_t Key(LPCVOID src, SIZE_T size, LPCSTR name, const void* defines, LPCSTR entry, LPCSTR target, UINT f1, UINT f2)
    {
        uint64_t h = 14695981039346656037ull;
        Mix(h, src, size);
        Mix(h, name ? name : "", name ? strlen(name) : 0);
        Mix(h, entry ? entry : "", entry ? strlen(entry) : 0);
        Mix(h, target ? target : "", target ? strlen(target) : 0);
        Mix(h, &f1, sizeof(f1));
        Mix(h, &f2, sizeof(f2));
        // D3D_SHADER_MACRO pairs, ended by a null name.
        if (const char* const* d = static_cast<const char* const*>(defines))
            for (; d[0]; d += 2)
            {
                Mix(h, d[0], strlen(d[0]));
                Mix(h, d[1] ? d[1] : "", d[1] ? strlen(d[1]) : 0);
            }
        return h;
    }

    std::wstring PathFor(uint64_t key)
    {
        wchar_t file[40];
        swprintf_s(file, L"%016llx.cso", static_cast<unsigned long long>(key));
        return g_dir + L"\\" + file;
    }

    bool ReadDisk(uint64_t key, std::vector<unsigned char>& out)
    {
        if (g_dir.empty())
            return false;
        FILE* f = nullptr;
        if (_wfopen_s(&f, PathFor(key).c_str(), L"rb") != 0 || !f)
            return false;
        fseek(f, 0, SEEK_END);
        const long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        bool ok = n > 0 && n < (1 << 24);
        if (ok)
        {
            out.resize(static_cast<size_t>(n));
            ok = fread(out.data(), 1, out.size(), f) == out.size();
        }
        fclose(f);
        return ok;
    }

    void WriteDisk(uint64_t key, const std::vector<unsigned char>& code)
    {
        if (g_dir.empty() || code.empty())
            return;
        CreateDirectoryW(g_dir.c_str(), nullptr);
        const std::wstring path = PathFor(key), tmp = path + L".tmp";
        FILE* f = nullptr;
        if (_wfopen_s(&f, tmp.c_str(), L"wb") != 0 || !f)
            return;
        const bool ok = fwrite(code.data(), 1, code.size(), f) == code.size();
        fclose(f);
        if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
            DeleteFileW(tmp.c_str());
    }

    OgBlob* MakeBlob(const void* data, size_t n)
    {
        static auto create = reinterpret_cast<PFN_D3DCreateBlob>(
            GetProcAddress(GetModuleHandleA("d3dcompiler_47.dll"), "D3DCreateBlob"));
        OgBlob* b = nullptr;
        if (!create || FAILED(create(n, &b)) || !b)
            return nullptr;
        memcpy(b->lpVtbl->GetBufferPointer(b), data, n);
        return b;
    }

    // Fills the entry for key: from the disk, else compiled. Called with the lock not held; the entry is pending
    // and belongs to this thread until it is marked ready or failed.
    void Produce(uint64_t key, PFN_D3DCompile real, LPCVOID src, SIZE_T size, LPCSTR name, const void* defines,
                 void* include, LPCSTR entry, LPCSTR target, UINT f1, UINT f2, const char* who)
    {
        const double t0 = Now();
        std::vector<unsigned char> code;
        std::string errors;
        HRESULT hr = S_OK;
        const char* from = "the disk";
        if (!ReadDisk(key, code))
        {
            from = "compiled";
            OgBlob* c = nullptr;
            OgBlob* e = nullptr;
            hr = real(src, size, name, defines, include, entry, target, f1, f2, &c, &e);
            if (SUCCEEDED(hr) && c)
            {
                const unsigned char* p = static_cast<const unsigned char*>(c->lpVtbl->GetBufferPointer(c));
                code.assign(p, p + c->lpVtbl->GetBufferSize(c));
                WriteDisk(key, code);
            }
            else if (SUCCEEDED(hr))
                hr = E_FAIL;
            if (e)
            {
                errors.assign(static_cast<const char*>(e->lpVtbl->GetBufferPointer(e)), e->lpVtbl->GetBufferSize(e));
                e->lpVtbl->Release(e);
            }
            if (c)
                c->lpVtbl->Release(c);
        }
        Log("shaders: %s (%s) %s in %.1f ms, by %s%s", name ? name : "(unnamed)", target ? target : "?",
            FAILED(hr) ? "FAILED" : from, (Now() - t0) * 1000.0, who, FAILED(hr) ? " (kept for the session)" : "");
        {
            std::lock_guard<std::mutex> lock(g_m);
            Entry& en = g_entries[key];
            en.state = FAILED(hr) ? Entry::kFailed : Entry::kReady;
            en.code = std::move(code);
            en.errors = std::move(errors);
            en.hr = hr;
        }
        g_cv.notify_all();
    }

    DWORD WINAPI Worker(LPVOID)
    {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        auto real = reinterpret_cast<PFN_D3DCompile>(
            GetProcAddress(GetModuleHandleA("d3dcompiler_47.dll"), "D3DCompile"));
        if (!real)
            return 0;
        const double t0 = Now();
        std::vector<Job> jobs;
        {
            std::lock_guard<std::mutex> lock(g_m);
            jobs.swap(g_jobs);
        }
        unsigned done = 0;
        for (const Job& j : jobs)
        {
            std::vector<const char*> defs;
            for (const std::string& d : j.defines)
                defs.push_back(d.c_str());
            defs.push_back(nullptr);
            defs.push_back(nullptr);
            const void* defines = j.defines.empty() ? nullptr : defs.data();
            const uint64_t key = Key(j.src.data(), j.src.size(), j.name.c_str(), defines, "main", j.profile.c_str(), 0, 0);
            {
                std::lock_guard<std::mutex> lock(g_m);
                if (g_entries.count(key))
                    continue;   // the game's thread has it already, or had it
                g_entries[key].state = Entry::kPending;
            }
            Produce(key, real, j.src.data(), j.src.size(), j.name.c_str(), defines, nullptr, "main", j.profile.c_str(),
                    0, 0, "the worker");
            ++done;
        }
        Log("shaders: the worker is done: %u of %u listed shaders in %.0f ms", done, static_cast<unsigned>(jobs.size()),
            (Now() - t0) * 1000.0);
        return 0;
    }
}

void ShaderPrecompile(const char* name, const char* src, const char* profile, const char* const* defines)
{
    Job j;
    j.name = name;
    j.src = src;
    j.profile = profile;
    if (defines)
        for (; defines[0]; defines += 2)
        {
            j.defines.push_back(defines[0]);
            j.defines.push_back(defines[1] ? defines[1] : "");
        }
    std::lock_guard<std::mutex> lock(g_m);
    g_jobs.push_back(std::move(j));
}

void ShaderCacheStart(const wchar_t* dir)
{
    g_dir = dir;
    // Load the compiler here, off the loader lock: the worker and the passes both find it loaded.
    if (!GetModuleHandleA("d3dcompiler_47.dll") && !LoadLibraryA("d3dcompiler_47.dll"))
    {
        Log("shaders: no d3dcompiler_47.dll: no cache, no worker");
        return;
    }
    // In the order the passes first need them: the rays, the sun shadows, the light, the cover, the water, the
    // lamps, the beacon; the small ones last.
    RaysShaderList();
    SunShadowsShaderList();
    VolumeShaderList();
    CoverShaderList();
    WaterShaderList();
    LampGlowShaderList();
    BeaconShaderList();
    GradeShaderList();
    BodyMaskShaderList();
    ShadowShaderList();
    HANDLE t = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    if (t)
        CloseHandle(t);
    else
        Log("shaders: could not start the worker");
}

HRESULT ShaderCacheCompile(PFN_D3DCompile real, LPCVOID src, SIZE_T size, LPCSTR name, const void* defines,
                           void* include, LPCSTR entry, LPCSTR target, UINT f1, UINT f2, OgBlob** code, OgBlob** errs)
{
    if (code)
        *code = nullptr;
    if (errs)
        *errs = nullptr;
    const uint64_t key = Key(src, size, name, defines, entry, target, f1, f2);
    bool mine = false;
    {
        std::unique_lock<std::mutex> lock(g_m);
        auto it = g_entries.find(key);
        if (it == g_entries.end())
        {
            g_entries[key].state = Entry::kPending;
            mine = true;
        }
        else if (it->second.state == Entry::kPending)
        {
            const double t0 = Now();
            g_cv.wait(lock, [&] { return g_entries[key].state != Entry::kPending; });
            Log("shaders: %s (%s) waited %.0f ms for the worker", name ? name : "(unnamed)", target ? target : "?",
                (Now() - t0) * 1000.0);
        }
    }
    if (mine)
        Produce(key, real, src, size, name, defines, include, entry, target, f1, f2, "the game's thread");
    std::lock_guard<std::mutex> lock(g_m);
    const Entry& en = g_entries[key];
    if (en.state == Entry::kReady)
    {
        if (code)
            *code = MakeBlob(en.code.data(), en.code.size());
        return code && !*code ? E_OUTOFMEMORY : S_OK;
    }
    if (errs && !en.errors.empty())
        *errs = MakeBlob(en.errors.c_str(), en.errors.size());
    return FAILED(en.hr) ? en.hr : E_FAIL;
}
