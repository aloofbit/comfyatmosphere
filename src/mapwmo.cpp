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

#include "mapm2.h"
#include "mapwmo.h"
#include "mpq.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <unordered_map>

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
            // The group's header: flags at +8, its box at +12. 0x2000: an indoor group (a room, a cellar).
            if (o + 8 + 36 <= end && (U32(d, o + 8 + 8) & 0x2000))
            {
                float box[6];
                memcpy(box, &d[o + 8 + 12], 24);
                out.indoor.insert(out.indoor.end(), box, box + 6);
            }
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

namespace
{
    constexpr uint32_t kMODN = 0x4D4F444E, kMODS = 0x4D4F4453, kMODD = 0x4D4F4444, kMOLT = 0x4D4F4C54;

    // The doodads that give light, by the name of their model, and how far their light reaches. Inside a
    // building the client has no point lights (probe, the Darkshire inn): its candles are particles, and the
    // lamps never saw them (2026-09-30). First match wins.
    struct LightWord { const char* word; float reach; };
    const LightWord kLightWords[] = {
        { "CHANDELIER", 10.0f }, { "CANDELABRA", 8.0f }, { "CANDLE", 6.0f }, { "SCONCE", 10.0f }, { "LANTERN", 10.0f },
        { "LAMP", 10.0f }, { "TORCH", 14.0f }, { "BRAZIER", 16.0f }, { "FIREPLACE", 16.0f },
        { "CAMPFIRE", 16.0f }, { "FIREPIT", 16.0f },
    };

    // The building's doodads that give light (MODN names, MODS sets, MODD placements: 40 bytes, the name's
    // offset in its low 24 bits, position, rotation as a quaternion x y z w, scale). The flame is taken at the
    // top of the model: its box centre across, 85% of the way up.
    void Lights(const std::vector<uint8_t>& d, WmoMesh& out)
    {
        size_t modn = 0, modnSize = 0, mods = 0, modsSize = 0, modd = 0, moddSize = 0;
        for (size_t o = 0; o + 8 <= d.size();)
        {
            const uint32_t tag = U32(d, o), size = U32(d, o + 4);
            if (o + 8 + static_cast<size_t>(size) > d.size())
                break;
            if (tag == kMODN) { modn = o + 8; modnSize = size; }
            if (tag == kMODS) { mods = o + 8; modsSize = size; }
            if (tag == kMODD) { modd = o + 8; moddSize = size; }
            o += 8 + static_cast<size_t>(size);
        }
        // The building's own lights (MOLT, 48 bytes: type, use attenuation, colour as BGRA, position,
        // intensity, 16 bytes unused, attenuation start and end). The client bakes the building's light from
        // them; a fireplace has one and no doodad named for it (the Goldshire inn: an orange light, 9.2 yards,
        // 2.8 yards from the owner standing at it, 2026-09-30). They come first, with their own colour and reach.
        for (size_t o = 0; o + 8 <= d.size();)
        {
            const uint32_t tag = U32(d, o), size = U32(d, o + 4);
            if (o + 8 + static_cast<size_t>(size) > d.size())
                break;
            if (tag == kMOLT)
                for (size_t k = o + 8; k + 48 <= o + 8 + size; k += 48)
                {
                    WmoLight L = {};
                    memcpy(L.pos, &d[k + 8], 12);
                    const uint32_t c = U32(d, k + 4);
                    float inten, end;
                    memcpy(&inten, &d[k + 20], 4);
                    memcpy(&end, &d[k + 44], 4);
                    if (!(inten > 0.0f) || !(end > 0.0f) || end > 200.0f)
                        continue;
                    inten = (std::min)(inten, 2.0f);
                    L.colour[0] = ((c >> 16) & 0xFF) / 255.0f * inten;
                    L.colour[1] = ((c >> 8) & 0xFF) / 255.0f * inten;
                    L.colour[2] = (c & 0xFF) / 255.0f * inten;
                    L.reach = (std::max)(end, 5.0f);
                    L.set = 0;
                    strncpy_s(L.what, "BUILDING", _TRUNCATE);
                    out.lights.push_back(L);
                }
            o += 8 + static_cast<size_t>(size);
        }
        const size_t own = out.lights.size();
        if (!modnSize || !moddSize)
            return;
        std::unordered_map<std::string, std::array<float, 4>> tops;   // a light model's flame, own space, and whether read
        const uint32_t nDoodads = static_cast<uint32_t>(moddSize / 40);
        for (uint32_t i = 0; i < nDoodads; ++i)
        {
            const size_t o = modd + i * 40ull;
            const uint32_t nameOff = U32(d, o) & 0xFFFFFF;
            if (nameOff >= modnSize)
                continue;
            std::string name;
            for (size_t k = modn + nameOff; k < modn + modnSize && d[k]; ++k)
                name += static_cast<char>(d[k] >= 'a' && d[k] <= 'z' ? d[k] - 32 : d[k]);
            const std::string file = name.substr(name.find_last_of('\\') + 1);
            const LightWord* w = nullptr;
            for (const LightWord& lw : kLightWords)
                if (file.find(lw.word) != std::string::npos) { w = &lw; break; }
            if (!w)
                continue;
            auto it = tops.find(name);
            if (it == tops.end())
            {
                M2Model m;
                float top[4] = { 0.0f, 0.0f, 0.5f, 0.0f };
                if (M2Load(name, m) && !m.pos.empty())
                {
                    float lo[3] = { 1e9f, 1e9f, 1e9f }, hi[3] = { -1e9f, -1e9f, -1e9f };
                    for (size_t k = 0; k + 2 < m.pos.size(); k += 3)
                        for (int j = 0; j < 3; ++j)
                        {
                            lo[j] = (std::min)(lo[j], m.pos[k + j]);
                            hi[j] = (std::max)(hi[j], m.pos[k + j]);
                        }
                    top[0] = (lo[0] + hi[0]) * 0.5f;
                    top[1] = (lo[1] + hi[1]) * 0.5f;
                    top[2] = lo[2] + (hi[2] - lo[2]) * 0.85f;
                    top[3] = 1.0f;
                }
                it = tops.emplace(name, std::array<float, 4>{ top[0], top[1], top[2], top[3] }).first;
            }
            float pos[3], q[4], scale;
            memcpy(pos, &d[o + 4], 12);
            memcpy(q, &d[o + 16], 16);
            memcpy(&scale, &d[o + 32], 4);
            if (!(scale > 0.0f && scale < 100.0f))
                scale = 1.0f;
            // v' = q v q*, for the flame's offset scaled.
            const float v[3] = { it->second[0] * scale, it->second[1] * scale, it->second[2] * scale };
            const float x = q[0], y = q[1], z = q[2], s = q[3];
            const float t[3] = { 2.0f * (y * v[2] - z * v[1]), 2.0f * (z * v[0] - x * v[2]), 2.0f * (x * v[1] - y * v[0]) };
            const float r[3] = { v[0] + s * t[0] + (y * t[2] - z * t[1]), v[1] + s * t[1] + (z * t[0] - x * t[2]),
                                 v[2] + s * t[2] + (x * t[1] - y * t[0]) };
            WmoLight L = {};
            for (int j = 0; j < 3; ++j)
                L.pos[j] = pos[j] + r[j];
            // Not where one of the building's own lights already is: a lantern's own light sits 2 yards
            // under the top of its model.
            bool near = false;
            for (size_t k = 0; k < own && !near; ++k)
            {
                const float dx = out.lights[k].pos[0] - L.pos[0], dy = out.lights[k].pos[1] - L.pos[1],
                            dz = out.lights[k].pos[2] - L.pos[2];
                near = dx * dx + dy * dy + dz * dz < 2.5f * 2.5f;
            }
            if (near)
                continue;
            L.colour[0] = 1.0f; L.colour[1] = 0.62f; L.colour[2] = 0.29f;
            L.reach = w->reach * (std::min)((std::max)(scale, 0.5f), 2.0f);
            L.set = 0;
            for (size_t sOff = mods; sOff + 32 <= mods + modsSize; sOff += 32)
            {
                const uint32_t first = U32(d, sOff + 20), count = U32(d, sOff + 24);
                if (i >= first && i < first + count)
                {
                    L.set = static_cast<uint16_t>((sOff - mods) / 32);
                    break;
                }
            }
            strncpy_s(L.what, w->word, _TRUNCATE);
            out.lights.push_back(L);
        }
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
    Lights(d, out);
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
