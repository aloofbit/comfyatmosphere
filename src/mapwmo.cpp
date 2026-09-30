// mapwmo: a building (WMO) read from the client's archives, for the shadow map.
//
// A WMO is a root file and one file for each group: "<root>_000.wmo" and on. The root's MOHD gives the
// group count and MOMT the materials (64 bytes each, the blend mode at +8). A group file holds MOGP: a
// 0x44-byte header and then its own chunks, among them MOPY (2 bytes a triangle: flags, material), MOVI
// (16-bit indices) and MOVT (vertices, in the building's own space, used as they are: the same as the
// core's vmap extractor, tools/vmap_extractor/vmapextract/wmo.cpp).
//
// Kept: the triangles drawn opaque. Material 0xFF marks a triangle that only collides and is never drawn.
// An alpha-keyed material (blend mode 1: a grate, a fence, a vine) is left out: the client's own draw of
// it keeps its cut-out shape in the cache, and a blended one (2 and up: glass, water) casts nothing.

#include "mapwmo.h"
#include "mpq.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace
{
    uint32_t U32(const std::vector<uint8_t>& d, size_t o) { uint32_t v; memcpy(&v, &d[o], 4); return v; }

    constexpr uint32_t kMOHD = 0x4D4F4844, kMOMT = 0x4D4F4D54, kMOGP = 0x4D4F4750;
    constexpr uint32_t kMOPY = 0x4D4F5059, kMOVI = 0x4D4F5649, kMOVT = 0x4D4F5654;

    bool Group(const std::vector<uint8_t>& d, const std::vector<uint32_t>& blend, WmoMesh& out)
    {
        for (size_t o = 0; o + 8 <= d.size();)
        {
            const uint32_t tag = U32(d, o), size = U32(d, o + 4);
            if (o + 8 + static_cast<size_t>(size) > d.size())
                return false;
            if (tag != kMOGP)
            {
                o += 8 + static_cast<size_t>(size);
                continue;
            }
            const size_t end = o + 8 + size;
            const uint8_t*  mopy = nullptr; size_t nTri = 0;
            const uint8_t*  movi = nullptr; size_t nIdx = 0;
            const uint8_t*  movt = nullptr; size_t nVert = 0;
            for (size_t s = o + 8 + 0x44; s + 8 <= end;)
            {
                const uint32_t t = U32(d, s), n = U32(d, s + 4);
                if (s + 8 + static_cast<size_t>(n) > end)
                    break;
                if (t == kMOPY) { mopy = &d[s + 8]; nTri = n / 2; }
                if (t == kMOVI) { movi = &d[s + 8]; nIdx = n / 2; }
                if (t == kMOVT) { movt = &d[s + 8]; nVert = n / 12; }
                s += 8 + static_cast<size_t>(n);
            }
            if (!movi || !movt || !mopy)
                return nVert == 0;   // a group with no geometry
            nTri = (std::min)(nTri, nIdx / 3);
            const uint32_t base = static_cast<uint32_t>(out.v.size() / 3);
            const size_t vFirst = out.v.size();
            out.v.resize(vFirst + nVert * 3);
            memcpy(&out.v[vFirst], movt, nVert * 12);
            for (size_t i = 0; i < nTri; ++i)
            {
                const uint8_t mat = mopy[i * 2 + 1];
                if (mat == 0xFF)
                    continue;   // collision only, never drawn
                if (mat >= blend.size() || blend[mat] != 0)
                {
                    ++out.other;
                    continue;
                }
                uint16_t tri[3];
                memcpy(tri, movi + i * 6, 6);
                if (tri[0] >= nVert || tri[1] >= nVert || tri[2] >= nVert)
                    continue;
                for (uint16_t k : tri)
                    out.idx.push_back(base + k);
                ++out.opaque;
            }
            return true;
        }
        return false;
    }
}

bool WmoLoad(const std::string& rootName, WmoMesh& out)
{
    out = WmoMesh();
    std::vector<uint8_t> d;
    if (!MpqRead(rootName.c_str(), d))
        return false;
    std::vector<uint32_t> blend;
    for (size_t o = 0; o + 8 <= d.size();)
    {
        const uint32_t tag = U32(d, o), size = U32(d, o + 4);
        if (o + 8 + static_cast<size_t>(size) > d.size())
            break;
        if (tag == kMOHD && size >= 60)
        {
            out.groups = U32(d, o + 8 + 4);
            memcpy(out.lo, &d[o + 8 + 36], 12);
            memcpy(out.hi, &d[o + 8 + 48], 12);
        }
        if (tag == kMOMT)
            for (size_t m = 0; m + 64 <= size; m += 64)
                blend.push_back(U32(d, o + 8 + m + 8));
        o += 8 + static_cast<size_t>(size);
    }
    if (out.groups == 0 || out.groups > 1024)
        return false;
    // "<root>_000.wmo": the root's name without its ".wmo".
    const size_t dot = rootName.size() >= 4 ? rootName.size() - 4 : rootName.size();
    const std::string stem = rootName.substr(0, dot);
    for (unsigned g = 0; g < out.groups; ++g)
    {
        char name[16];
        _snprintf_s(name, sizeof(name), _TRUNCATE, "_%03u.wmo", g);
        if (MpqRead((stem + name).c_str(), d) && Group(d, blend, out))
            ++out.groupsRead;
    }
    return out.groupsRead > 0 && !out.idx.empty();
}
