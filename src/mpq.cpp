// mpq: reading files out of the client's MPQ archives.
//
// Enough of the format for the terrain (mapterrain.cpp): the header, the two encrypted tables, and
// sectors stored plain or zlib-compressed. The same subset as tools/model-browser/lib/mpq.js, which was
// checked against the core's extractor. Measured on 2026-09-29: every .adt in terrain.MPQ, patch.MPQ and
// patch-2 to patch-9 is zlib (sector mask 0x02) and none is encrypted. PKWARE implode, bzip2 and
// encrypted files are refused, and the caller then goes without.
//
// Which archive wins: base archives < patch.MPQ < patch-0..9 < patch-A..Z, the order client.js uses. For
// the terrain it is only the numbered patches over patch.MPQ over terrain.MPQ: no lettered patch holds a
// tile. A file named `.mpq.off` or `.mpq.part` is not an archive.

#define WIN32_LEAN_AND_MEAN

#include <windows.h>

#include "common.h"
#include "mpq.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <string>

namespace
{
    // ---------------------------------------------------------------------------------------------
    // inflate (RFC 1951), after zlib's puff.c: slow but small, and the loader runs on its own thread.

    struct Huff
    {
        short count[16];
        short symbol[320];
    };

    struct Inflater
    {
        const uint8_t*        in;
        size_t                inLen, pos = 0;
        uint32_t              buf = 0;
        int                   cnt = 0;
        std::vector<uint8_t>& out;
        bool                  err = false;
        size_t                start;   // this stream's first byte in `out`: nothing before it is referred to

        Inflater(const uint8_t* p, size_t n, std::vector<uint8_t>& o) : in(p), inLen(n), out(o), start(o.size()) {}

        int Bits(int need)
        {
            uint32_t v = buf;
            while (cnt < need)
            {
                if (pos >= inLen) { err = true; return 0; }
                v |= static_cast<uint32_t>(in[pos++]) << cnt;
                cnt += 8;
            }
            buf = v >> need;
            cnt -= need;
            return static_cast<int>(v & ((1u << need) - 1));
        }

        int Decode(const Huff& h)
        {
            int code = 0, first = 0, index = 0;
            for (int len = 1; len < 16; ++len)
            {
                code |= Bits(1);
                if (err)
                    return -1;
                const int count = h.count[len];
                if (code - count < first)
                    return h.symbol[index + (code - first)];
                index += count;
                first += count;
                first <<= 1;
                code <<= 1;
            }
            return -1;
        }

        static int Construct(Huff& h, const short* length, int n)
        {
            for (int len = 0; len < 16; ++len)
                h.count[len] = 0;
            for (int s = 0; s < n; ++s)
                h.count[length[s]]++;
            if (h.count[0] == n)
                return 0;
            int left = 1;
            for (int len = 1; len < 16; ++len)
            {
                left <<= 1;
                left -= h.count[len];
                if (left < 0)
                    return left;
            }
            short offs[16];
            offs[1] = 0;
            for (int len = 1; len < 15; ++len)
                offs[len + 1] = static_cast<short>(offs[len] + h.count[len]);
            for (int s = 0; s < n; ++s)
                if (length[s] != 0)
                    h.symbol[offs[length[s]]++] = static_cast<short>(s);
            return left;
        }

        bool Stored()
        {
            buf = 0;
            cnt = 0;
            if (pos + 4 > inLen)
                return false;
            const unsigned len  = in[pos] | (in[pos + 1] << 8);
            const unsigned nlen = in[pos + 2] | (in[pos + 3] << 8);
            pos += 4;
            if (len != (~nlen & 0xFFFF) || pos + len > inLen)
                return false;
            out.insert(out.end(), in + pos, in + pos + len);
            pos += len;
            return true;
        }

        bool Codes(const Huff& lencode, const Huff& distcode)
        {
            static const short lbase[29] = { 3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                             31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
            static const short lext[29]  = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                             2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
            static const short dbase[30] = { 1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,    65,    97,    129,
                                             193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
            static const short dext[30]  = { 0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                             6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
            for (;;)
            {
                int sym = Decode(lencode);
                if (sym < 0 || err)
                    return false;
                if (sym < 256)
                    out.push_back(static_cast<uint8_t>(sym));
                else if (sym == 256)
                    return true;
                else
                {
                    sym -= 257;
                    if (sym >= 29)
                        return false;
                    const int len = lbase[sym] + Bits(lext[sym]);
                    sym = Decode(distcode);
                    if (sym < 0 || sym >= 30 || err)
                        return false;
                    const size_t dist = static_cast<size_t>(dbase[sym] + Bits(dext[sym]));
                    if (err || dist > out.size() - start)
                        return false;
                    const size_t from = out.size() - dist;
                    for (int i = 0; i < len; ++i)
                        out.push_back(out[from + i]);
                }
            }
        }

        bool Fixed()
        {
            static Huff lencode, distcode;
            static bool built = false;
            if (!built)
            {
                short lengths[288 + 30];
                int s = 0;
                for (; s < 144; ++s) lengths[s] = 8;
                for (; s < 256; ++s) lengths[s] = 9;
                for (; s < 280; ++s) lengths[s] = 7;
                for (; s < 288; ++s) lengths[s] = 8;
                Construct(lencode, lengths, 288);
                for (s = 0; s < 30; ++s) lengths[s] = 5;
                Construct(distcode, lengths, 30);
                built = true;
            }
            return Codes(lencode, distcode);
        }

        bool Dynamic()
        {
            static const short order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
            short lengths[320];
            const int nlen  = Bits(5) + 257;
            const int ndist = Bits(5) + 1;
            const int ncode = Bits(4) + 4;
            if (err || nlen > 286 || ndist > 30)
                return false;
            int i = 0;
            for (; i < ncode; ++i) lengths[order[i]] = static_cast<short>(Bits(3));
            for (; i < 19; ++i) lengths[order[i]] = 0;
            Huff lencode, distcode;
            if (err || Construct(lencode, lengths, 19) != 0)
                return false;
            for (i = 0; i < nlen + ndist;)
            {
                int sym = Decode(lencode);
                if (sym < 0 || err)
                    return false;
                if (sym < 16)
                {
                    lengths[i++] = static_cast<short>(sym);
                    continue;
                }
                short len = 0;
                int rep;
                if (sym == 16)
                {
                    if (i == 0)
                        return false;
                    len = lengths[i - 1];
                    rep = 3 + Bits(2);
                }
                else if (sym == 17)
                    rep = 3 + Bits(3);
                else
                    rep = 11 + Bits(7);
                if (err || i + rep > nlen + ndist)
                    return false;
                while (rep--)
                    lengths[i++] = len;
            }
            if (lengths[256] == 0)
                return false;
            if (Construct(lencode, lengths, nlen) < 0)
                return false;
            if (Construct(distcode, lengths + nlen, ndist) < 0)
                return false;
            return Codes(lencode, distcode);
        }

        bool Run()
        {
            int last;
            do
            {
                last = Bits(1);
                const int type = Bits(2);
                if (err)
                    return false;
                const bool ok = type == 0 ? Stored() : type == 1 ? Fixed() : type == 2 ? Dynamic() : false;
                if (!ok)
                    return false;
            } while (!last);
            return true;
        }
    };

    // A zlib stream (two header bytes, deflate, adler) into `out`, which is appended to.
    bool Unzlib(const uint8_t* p, size_t n, std::vector<uint8_t>& out)
    {
        if (n < 2 || (p[0] & 0x0F) != 8 || ((p[0] << 8) | p[1]) % 31 != 0 || (p[1] & 0x20))
            return false;
        Inflater f(p + 2, n - 2, out);
        return f.Run();
    }

    // ---------------------------------------------------------------------------------------------
    // the archive format

    uint32_t g_crypt[0x500];

    void BuildCrypt()
    {
        uint32_t seed = 0x00100001;
        for (int i1 = 0; i1 < 0x100; ++i1)
            for (int i2 = i1, i = 0; i < 5; ++i, i2 += 0x100)
            {
                seed = (seed * 125 + 3) % 0x2AAAAB;
                const uint32_t t1 = (seed & 0xFFFF) << 16;
                seed = (seed * 125 + 3) % 0x2AAAAB;
                g_crypt[i2] = t1 | (seed & 0xFFFF);
            }
    }

    // type 0 = table offset, 1 = name A, 2 = name B, 3 = file key.
    uint32_t HashString(const char* s, uint32_t type)
    {
        uint32_t s1 = 0x7FED7FED, s2 = 0xEEEEEEEE;
        for (; *s; ++s)
        {
            uint32_t ch = static_cast<uint8_t>(*s);
            if (ch >= 'a' && ch <= 'z')
                ch -= 32;
            if (ch == '/')
                ch = '\\';
            s1 = g_crypt[(type << 8) + ch] ^ (s1 + s2);
            s2 = ch + s1 + s2 + (s2 << 5) + 3;
        }
        return s1;
    }

    void Decrypt(uint32_t* p, size_t words, uint32_t key)
    {
        uint32_t s2 = 0xEEEEEEEE;
        for (size_t i = 0; i < words; ++i)
        {
            s2 += g_crypt[0x400 + (key & 0xFF)];
            const uint32_t ch = p[i] ^ (key + s2);
            p[i] = ch;
            key = ((~key << 0x15) + 0x11111111) | (key >> 0x0B);
            s2 = ch + s2 + (s2 << 5) + 3;
        }
    }

    const uint32_t kExists    = 0x80000000;
    const uint32_t kImplode   = 0x00000100;
    const uint32_t kCompress  = 0x00000200;
    const uint32_t kEncrypted = 0x00010000;
    const uint32_t kSingle    = 0x01000000;
    const uint32_t kDeleted   = 0x02000000;

    struct Archive
    {
        std::wstring          path;
        std::string           name;   // for the log
        int                   rank = 0;
        HANDLE                file = INVALID_HANDLE_VALUE;
        uint64_t              base = 0;
        uint32_t              sectorSize = 0;
        std::vector<uint32_t> hash, block;   // 4 words an entry each

        ~Archive()
        {
            if (file != INVALID_HANDLE_VALUE)
                CloseHandle(file);
        }

        bool ReadAt(uint64_t off, void* dst, uint32_t n) const
        {
            LARGE_INTEGER li;
            li.QuadPart = static_cast<LONGLONG>(off);
            DWORD got = 0;
            return SetFilePointerEx(file, li, nullptr, FILE_BEGIN) && ReadFile(file, dst, n, &got, nullptr) && got == n;
        }

        bool Open()
        {
            file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE)
                return false;
            LARGE_INTEGER size;
            if (!GetFileSizeEx(file, &size))
                return false;
            // The header is 512-byte aligned, and not always at offset 0.
            uint8_t hdr[32];
            uint64_t off = 0;
            for (; off + 32 <= static_cast<uint64_t>(size.QuadPart); off += 512)
            {
                if (!ReadAt(off, hdr, 32))
                    return false;
                uint32_t sig;
                memcpy(&sig, hdr, 4);
                if (sig == 0x1A51504D)
                    break;
            }
            if (off + 32 > static_cast<uint64_t>(size.QuadPart))
                return false;
            base = off;
            uint16_t shift;
            uint32_t hashPos, blockPos, hashCount, blockCount;
            memcpy(&shift, hdr + 0x0E, 2);
            memcpy(&hashPos, hdr + 0x10, 4);
            memcpy(&blockPos, hdr + 0x14, 4);
            memcpy(&hashCount, hdr + 0x18, 4);
            memcpy(&blockCount, hdr + 0x1C, 4);
            if (shift > 20 || hashCount == 0 || hashCount > (1u << 20) || blockCount > (1u << 20))
                return false;
            sectorSize = 512u << shift;
            hash.resize(static_cast<size_t>(hashCount) * 4);
            block.resize(static_cast<size_t>(blockCount) * 4);
            if (!ReadAt(base + hashPos, hash.data(), hashCount * 16) ||
                (blockCount && !ReadAt(base + blockPos, block.data(), blockCount * 16)))
                return false;
            Decrypt(hash.data(), hash.size(), HashString("(hash table)", 3));
            Decrypt(block.data(), block.size(), HashString("(block table)", 3));
            return true;
        }

        // The block index for a name, or -1.
        int Find(const char* n) const
        {
            const uint32_t count = static_cast<uint32_t>(hash.size() / 4);
            const uint32_t start = HashString(n, 0) % count;
            const uint32_t a = HashString(n, 1), b = HashString(n, 2);
            for (uint32_t i = 0; i < count; ++i)
            {
                const uint32_t* e = &hash[((start + i) % count) * 4];
                if (e[3] == 0xFFFFFFFF)
                    return -1;   // never used: stop probing
                if (e[0] == a && e[1] == b)
                    return static_cast<int>(e[3]);
            }
            return -1;
        }

        // 1 = read, 0 = not in this archive, -1 = here but not readable.
        int Read(const char* n, std::vector<uint8_t>& out) const
        {
            const int bi = Find(n);
            if (bi < 0 || static_cast<size_t>(bi) >= block.size() / 4)
                return 0;
            const uint32_t* b = &block[static_cast<size_t>(bi) * 4];
            const uint32_t filePos = b[0], cSize = b[1], fSize = b[2], flags = b[3];
            if (!(flags & kExists) || (flags & kDeleted))
                return 0;
            if ((flags & kEncrypted) || cSize > (256u << 20) || fSize > (256u << 20))
                return -1;
            std::vector<uint8_t> raw(cSize);
            if (cSize && !ReadAt(base + filePos, raw.data(), cSize))
                return -1;
            out.clear();
            if (!(flags & (kCompress | kImplode)))
            {
                if (fSize > cSize)
                    return -1;
                out.assign(raw.begin(), raw.begin() + fSize);
                return 1;
            }
            out.reserve(fSize);
            auto sector = [&](const uint8_t* p, size_t n, size_t want) {
                if (n >= want)   // a sector that did not get smaller is stored as it is
                {
                    out.insert(out.end(), p, p + want);
                    return true;
                }
                if (!(flags & kCompress) || n < 1 || p[0] != 0x02)
                    return false;   // implode, bzip2 and the rest: not needed for the terrain
                const size_t before = out.size();
                return Unzlib(p + 1, n - 1, out) && out.size() - before == want;
            };
            if (flags & kSingle)
                return sector(raw.data(), raw.size(), fSize) ? 1 : -1;
            const size_t nSectors = (static_cast<size_t>(fSize) + sectorSize - 1) / sectorSize;
            if ((nSectors + 1) * 4 > raw.size())
                return -1;
            std::vector<uint32_t> table(nSectors + 1);
            memcpy(table.data(), raw.data(), table.size() * 4);
            for (size_t i = 0; i < nSectors; ++i)
            {
                const size_t want = (std::min)(static_cast<size_t>(sectorSize), fSize - out.size());
                if (table[i] > table[i + 1] || table[i + 1] > raw.size() ||
                    !sector(raw.data() + table[i], table[i + 1] - table[i], want))
                    return -1;
            }
            return out.size() == fSize ? 1 : -1;
        }
    };

    std::vector<std::unique_ptr<Archive>> g_archives;   // best first
    bool g_opened = false;
    std::atomic<unsigned> g_count{ 0 };   // read by the render thread, for the probe

    int Rank(const std::string& lower)
    {
        if (lower == "patch.mpq")
            return 1;
        if (lower.size() == 11 && lower.compare(0, 6, "patch-") == 0 && lower.compare(7, 4, ".mpq") == 0)
        {
            char c = lower[6];
            if (c >= 'a' && c <= 'z')
                c = static_cast<char>(c - 32);
            return 2 + static_cast<unsigned char>(c);   // '0'..'9' then 'A'..'Z'
        }
        return 0;
    }
}

bool MpqOpen()
{
    wchar_t exe[MAX_PATH];
    const DWORD len = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (!len || len >= MAX_PATH)
        return false;
    std::wstring dir(exe);
    dir.erase(dir.find_last_of(L"\\/") + 1);
    dir += L"Data\\";
    return MpqOpenDir(dir.c_str());
}

bool MpqOpenDir(const wchar_t* dataDir)
{
    if (g_opened)
        return !g_archives.empty();
    g_opened = true;
    BuildCrypt();
    const std::wstring dir(dataDir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"*.mpq").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
    {
        Log("mpq: no archives in the Data folder");
        return false;
    }
    unsigned failed = 0;
    do
    {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        std::string name;
        for (const wchar_t* p = fd.cFileName; *p; ++p)
            name += (*p < 128) ? static_cast<char>(*p) : '?';
        std::string lower = name;
        for (char& c : lower)
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c + 32);
        // "*.mpq" also matches "x.mpq.off" through its short name, so the extension is checked again.
        if (lower.size() < 4 || lower.compare(lower.size() - 4, 4, ".mpq") != 0)
            continue;
        auto a = std::make_unique<Archive>();
        a->path = dir + fd.cFileName;
        a->name = name;
        a->rank = Rank(lower);
        if (a->Open())
            g_archives.push_back(std::move(a));
        else
            ++failed;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::stable_sort(g_archives.begin(), g_archives.end(),
                     [](const std::unique_ptr<Archive>& a, const std::unique_ptr<Archive>& b) { return a->rank > b->rank; });
    g_count = static_cast<unsigned>(g_archives.size());
    Log("mpq: %u archives open, %u could not be read", static_cast<unsigned>(g_archives.size()), failed);
    return !g_archives.empty();
}

bool MpqRead(const char* name, std::vector<uint8_t>& out)
{
    for (const auto& a : g_archives)
    {
        const int r = a->Read(name, out);
        if (r > 0)
            return true;
        if (r < 0)
        {
            // Here, but not readable: a lower archive's copy would be an older version, so none is used.
            Log("mpq: %s holds %s in a form this reader does not handle", a->name.c_str(), name);
            return false;
        }
    }
    return false;
}

std::string MpqHolders(const char* name)
{
    std::string s;
    for (const auto& a : g_archives)
        if (a->Find(name) >= 0)
        {
            if (!s.empty())
                s += ", ";
            s += a->name;
        }
    return s;
}

unsigned MpqArchiveCount() { return g_count; }
