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
    constexpr uint32_t kMOPY = 0x4D4F5059, kMOVI = 0x4D4F5649, kMOVT = 0x4D4F5654, kMOTX = 0x4D4F5458;

    // glass: the materials whose texture is a lamp's glass; the centre of each triangle drawn with one goes into
    // glassPts (see LampsInGeometry).
    bool Group(const std::vector<uint8_t>& d, const std::vector<uint32_t>& blend, const std::vector<bool>& glass,
               std::vector<float>& glassPts, WmoMesh& out)
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
            // 0x80: unreachable. No portal leads to it, so the client never draws it. Stormwind has four, 100
            // to 250 yards up over the city ("Mage Quarter", "Command Center", "garrison_hall", "HB03"), and
            // they cast a large shadow over the Trade District from an empty sky (2026-09-30). Left out whole.
            if (o + 8 + 36 <= end && (U32(d, o + 8 + 8) & 0x80))
                return true;
            // Not indoor when the group is also lit by the exterior light (0x40) or marked exterior (0x8): the
            // client lights it as it lights the open air. Stormwind's canal tunnels are 0xa040, and walking through
            // them switched every indoor rule on and off (2026-10-01).
            const uint32_t flags = o + 8 + 36 <= end ? U32(d, o + 8 + 8) : 0;
            const bool indoor = (flags & 0x2000) && !(flags & 0x48);
            if (indoor)
            {
                float box[6];
                memcpy(box, &d[o + 8 + 12], 24);
                out.indoor.insert(out.indoor.end(), box, box + 6);
                out.indoorTris.emplace_back();
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
            // An indoor group keeps all its triangles, collision ones too, for the ceiling test.
            if (indoor)
            {
                std::vector<float>& t = out.indoorTris.back();
                for (size_t i = 0; i < nIdx / 3; ++i)
                {
                    uint16_t tri[3];
                    memcpy(tri, movi + i * 6, 6);
                    if (tri[0] >= nVert || tri[1] >= nVert || tri[2] >= nVert)
                        continue;
                    for (uint16_t k : tri)
                    {
                        float p[3];
                        memcpy(p, movt + k * 12, 12);
                        t.insert(t.end(), p, p + 3);
                    }
                }
            }
            const uint32_t base = static_cast<uint32_t>(out.v.size() / 3);
            const size_t vFirst = out.v.size();
            out.v.resize(vFirst + nVert * 3);
            memcpy(&out.v[vFirst], movt, nVert * 12);
            for (size_t i = 0; i < nTri; ++i)
            {
                const uint8_t mat = mopy[i * 2 + 1];
                if (mat == 0xFF)
                    continue;   // collision only, never drawn
                if (mat < glass.size() && glass[mat])
                {
                    uint16_t tri[3];
                    memcpy(tri, movi + i * 6, 6);
                    if (tri[0] < nVert && tri[1] < nVert && tri[2] < nVert)
                    {
                        float c[3] = {};
                        for (uint16_t k : tri)
                            for (int j = 0; j < 3; ++j)
                            {
                                float v;
                                memcpy(&v, movt + k * 12 + j * 4, 4);
                                c[j] += v / 3.0f;
                            }
                        glassPts.insert(glassPts.end(), c, c + 3);
                    }
                }
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
        { "CAMPFIRE", 16.0f }, { "FIREPIT", 16.0f }, { "BONFIRE", 16.0f },
    };

    // What one doodad model gives as light, read once for every building (the loader thread alone runs this):
    // its word (null = none), reach, flames and colour. A model with no light word is read to see whether it
    // burns, so each model a building places is read once (2026-10-01).
    struct LightModelInfo
    {
        const char* word = nullptr;
        float       reach = 0.0f;
        int         count = 0;
        float       pos[kMaxFlames][3] = {};
        float       colour[3] = {};
    };
    std::unordered_map<std::string, LightModelInfo> g_lightModels;

    const LightModelInfo& LightModelOf(const std::string& name)
    {
        auto it = g_lightModels.find(name);
        if (it != g_lightModels.end())
            return it->second;
        LightModelInfo info;
        const std::string file = name.substr(name.find_last_of('\\') + 1);
        M2Model m;
        const bool read = M2Load(name, m);
        float reach = 0.0f;
        if (const char* word = LightModelWord(file, read ? &m : nullptr, reach))
        {
            info.word  = word;
            info.reach = reach;
            info.count = LightFlames(read ? &m : nullptr, strcmp(word, "FLAME") != 0, info.pos, info.colour);
        }
        return g_lightModels.emplace(name, info).first->second;
    }

    // The building's doodads that give light (MODN names, MODS sets, MODD placements: 40 bytes, the name's
    // offset in its low 24 bits, position, rotation as a quaternion x y z w, scale). The flame and its colour
    // come from the model (M2Load). Until 2026-10-01 every flame was orange and sat 85% of the way up the
    // model's box, which put the light 70 yards above an Undercity lantern (its chain is in the box) and 1.5
    // yards above the candles of the Goldshire inn's chandelier.
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
        std::vector<bool> replaced(own, false);
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
            const LightModelInfo& info = LightModelOf(name);
            if (!info.word)
                continue;
            float pos[3], q[4], scale;
            memcpy(pos, &d[o + 4], 12);
            memcpy(q, &d[o + 16], 16);
            memcpy(&scale, &d[o + 32], 4);
            if (!(scale > 0.0f && scale < 100.0f))
                scale = 1.0f;
            uint16_t set = 0;
            for (size_t sOff = mods; sOff + 32 <= mods + modsSize; sOff += 32)
            {
                const uint32_t first = U32(d, sOff + 20), count = U32(d, sOff + 24);
                if (i >= first && i < first + count)
                {
                    set = static_cast<uint16_t>((sOff - mods) / 32);
                    break;
                }
            }
            for (int f = 0; f < info.count; ++f)
            {
                // v' = q v q*, for the flame's offset scaled.
                const float v[3] = { info.pos[f][0] * scale, info.pos[f][1] * scale, info.pos[f][2] * scale };
                const float x = q[0], y = q[1], z = q[2], w = q[3];
                const float t[3] = { 2.0f * (y * v[2] - z * v[1]), 2.0f * (z * v[0] - x * v[2]), 2.0f * (x * v[1] - y * v[0]) };
                const float r[3] = { v[0] + w * t[0] + (y * t[2] - z * t[1]), v[1] + w * t[1] + (z * t[0] - x * t[2]),
                                     v[2] + w * t[2] + (x * t[1] - y * t[0]) };
                WmoLight L = {};
                for (int j = 0; j < 3; ++j)
                    L.pos[j] = pos[j] + r[j];
                // The building's own light at the same lamp (a lantern's sits 2 yards under the top of its
                // model) gives way to the flame. Until 2026-10-01 it was the other way round, and most of
                // Stormwind's torches showed as their building lights: (0.26 0.19 0.10), reaching 5 yards.
                for (size_t k = 0; k < own; ++k)
                {
                    const float dx = out.lights[k].pos[0] - L.pos[0], dy = out.lights[k].pos[1] - L.pos[1],
                                dz = out.lights[k].pos[2] - L.pos[2];
                    if (dx * dx + dy * dy + dz * dz < 2.5f * 2.5f)
                        replaced[k] = true;
                }
                memcpy(L.colour, info.colour, sizeof(L.colour));
                L.reach = info.reach * (std::min)((std::max)(scale, 0.5f), 2.0f);
                L.set = set;
                strncpy_s(L.what, info.word, _TRUNCATE);
                out.lights.push_back(L);
            }
        }
        std::vector<WmoLight> kept;
        kept.reserve(out.lights.size());
        for (size_t k = 0; k < out.lights.size(); ++k)
            if (k >= own || !replaced[k])
                kept.push_back(out.lights[k]);
        out.lights.swap(kept);
    }
}

const char* LightModelWord(const std::string& file, const M2Model* m, float& reach)
{
    // An unlit candle (CANDLEOFF01: 69 in Stormwind) and a broken lamppost give no light. Both have no flame
    // in the file either.
    if (file.find("OFF") != std::string::npos || file.find("BROKEN") != std::string::npos)
        return nullptr;
    for (const LightWord& lw : kLightWords)
        if (file.find(lw.word) != std::string::npos)
        {
            reach = lw.reach;
            return lw.word;
        }
    if (m && !m->firePts.empty())
    {
        reach = 14.0f;
        return "FLAME";
    }
    return nullptr;
}

int LightFlames(const M2Model* m, bool byWord, float pos[][3], float colour[3])
{
    // The orange the lights had before the colour was read, for a model that names none.
    colour[0] = 1.0f; colour[1] = 0.62f; colour[2] = 0.29f;
    pos[0][0] = pos[0][1] = 0.0f;
    pos[0][2] = 0.5f;
    if (!m || m->pos.empty())
        return 1;
    if (m->haveColour)
        memcpy(colour, m->flameColour, 3 * sizeof(float));
    // A model taken by its name: its glow quads, else its emitters. One taken by its flames: those alone, since
    // a building's small blended meshes (a window) are no flame.
    const std::vector<float>& pts = !byWord ? m->firePts : !m->quadPts.empty() ? m->quadPts : m->emitPts;
    const size_t n = pts.size() / 3;
    if (!n)
    {
        // Without a flame, near the top of the model (a pole torch: FREESTANDINGTORCH02), or for a tall one its
        // body: an Undercity lantern's chain reaches 84 yards up, so that box's top is no use.
        memcpy(pos[0], m->height <= 6.0f ? m->top : m->middle, 3 * sizeof(float));
        return 1;
    }
    // Points within 3 yards of each other, directly or through others, are one light at their middle.
    std::vector<int> group(n, -1);
    int groups = 0;
    for (size_t i = 0; i < n; ++i)
    {
        if (group[i] >= 0)
            continue;
        group[i] = groups;
        for (bool grew = true; grew;)
        {
            grew = false;
            for (size_t a = 0; a < n; ++a)
                if (group[a] == groups)
                    for (size_t b = 0; b < n; ++b)
                    {
                        if (group[b] >= 0)
                            continue;
                        const float dx = pts[a * 3] - pts[b * 3], dy = pts[a * 3 + 1] - pts[b * 3 + 1],
                                    dz = pts[a * 3 + 2] - pts[b * 3 + 2];
                        if (dx * dx + dy * dy + dz * dz < 9.0f)
                        {
                            group[b] = groups;
                            grew = true;
                        }
                    }
        }
        ++groups;
    }
    const int out = (std::min)(groups, kMaxFlames);
    for (int g = 0; g < out; ++g)
    {
        float sum[3] = {};
        int count = 0;
        for (size_t i = 0; i < n; ++i)
            if (group[i] == g)
            {
                for (int j = 0; j < 3; ++j)
                    sum[j] += pts[i * 3 + j];
                ++count;
            }
        for (int j = 0; j < 3; ++j)
            pos[g][j] = sum[j] / count;
    }
    return out;
}

// Lamps built into a building's own walls (2026-10-01). Stormwind's street lamps in this client (its root from
// patch-3.mpq) are part of the city's geometry, drawn with STORMWINDSTREETLAMP.BLP and STORMWINDLAMPGLASS.BLP:
// no doodad, game object or building light stands at them, so they had no light. Each group of glass triangles
// within 1.5 yards of each other is one lamp, lit like a Stormwind lamppost's glow, (0.92 0.74 0.22).
static void LampsInGeometry(const std::vector<float>& pts, WmoMesh& out)
{
    const size_t n = pts.size() / 3;
    std::vector<int> group(n, -1);
    int groups = 0;
    for (size_t i = 0; i < n; ++i)
    {
        if (group[i] >= 0)
            continue;
        std::vector<size_t> open{ i };
        group[i] = groups;
        float sum[3] = {};
        int count = 0;
        while (!open.empty())
        {
            const size_t a = open.back();
            open.pop_back();
            for (int j = 0; j < 3; ++j)
                sum[j] += pts[a * 3 + j];
            ++count;
            for (size_t b = 0; b < n; ++b)
            {
                if (group[b] >= 0)
                    continue;
                const float dx = pts[a * 3] - pts[b * 3], dy = pts[a * 3 + 1] - pts[b * 3 + 1],
                            dz = pts[a * 3 + 2] - pts[b * 3 + 2];
                if (dx * dx + dy * dy + dz * dz < 1.5f * 1.5f)
                {
                    group[b] = groups;
                    open.push_back(b);
                }
            }
        }
        ++groups;
        WmoLight L = {};
        for (int j = 0; j < 3; ++j)
            L.pos[j] = sum[j] / count;
        L.colour[0] = 1.0f; L.colour[1] = 0.80f; L.colour[2] = 0.24f;
        L.reach = 10.0f;
        L.set = 0;
        strncpy_s(L.what, "LAMP", _TRUNCATE);
        out.lights.push_back(L);
    }
}

bool WmoLoad(const std::string& rootName, WmoMesh& out)
{
    out = WmoMesh();
    std::vector<uint8_t> d;
    if (!MpqRead(rootName.c_str(), d))
        return false;
    std::vector<uint32_t> blend, texOff;
    std::string motx;
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
            {
                blend.push_back(U32(d, o + 8 + m + 8));
                texOff.push_back(U32(d, o + 8 + m + 12));   // its first texture, an offset into MOTX
            }
        if (tag == kMOTX)
            motx.assign(reinterpret_cast<const char*>(&d[o + 8]), size);
        o += 8 + static_cast<size_t>(size);
    }
    std::vector<bool> glass(texOff.size(), false);
    for (size_t m = 0; m < texOff.size(); ++m)
        if (texOff[m] < motx.size())
        {
            std::string t(motx.c_str() + texOff[m]);
            for (char& ch : t)
                ch = ch >= 'a' && ch <= 'z' ? static_cast<char>(ch - 32) : ch;
            glass[m] = t.find("LAMPGLASS") != std::string::npos;
        }
    std::vector<float> glassPts;
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
        if (MpqRead((stem + name).c_str(), d) && Group(d, blend, glass, glassPts, out))
            ++out.groupsRead;
    }
    LampsInGeometry(glassPts, out);
    return out.groupsRead > 0 && !out.idx.empty();
}
