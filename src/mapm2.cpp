// mapm2: doodad models (M2) and their textures (BLP) read from the client's archives, for the shadow map.
//
// A port of the parts of tools/model-browser/lib/m2.js and blp.js the shadow map needs, where each
// header offset was found and checked against this client's files; see those files for how.
//
//   M2      Version 256 (257 on a few), with its views inside the file. Vertices are 48 bytes, the
//           position first; view 0 is the most detailed. A view's triangles index its own vertex list,
//           which indexes the model's vertices. Each submesh is painted by the first texture unit that
//           names it: the unit's word 5 indexes the render flags (0x84: flags, blend mode), its word 8
//           the texture lookup (0x94), which indexes the textures (0x5C: type, name). A texture of
//           type 0 names its file; the others are filled in by the game (a creature's skin) and are
//           not used here. Positions are used as they are: the core's vmap extractor turns them and
//           back again (tools/vmap_extractor/vmapextract/model.cpp).
//   BLP     BLP2. Encoding 2 is DXT (alpha encoding 0 DXT1, 1 DXT3, 7 DXT5), handed to the GPU as it
//           is; encoding 1 is a 256-colour palette with 0, 1, 4 or 8 bits of alpha, and 3 is BGRA;
//           both become A8R8G8B8. Around Northshire 212 of the 217 leaf textures are DXT.

#include "mapm2.h"
#include "mpq.h"

#include <algorithm>
#include <cstring>

namespace
{
    uint32_t U32(const std::vector<uint8_t>& d, size_t o) { uint32_t v; memcpy(&v, &d[o], 4); return v; }
    uint16_t U16(const std::vector<uint8_t>& d, size_t o) { uint16_t v; memcpy(&v, &d[o], 2); return v; }

    // A texture name as a model gives it: some carry a trailing space, and some no extension.
    std::string TextureName(const std::vector<uint8_t>& d, uint32_t ofs, uint32_t len)
    {
        std::string s;
        for (uint32_t i = 0; i < len && ofs + i < d.size() && d[ofs + i]; ++i)
            s += static_cast<char>(d[ofs + i]);
        while (!s.empty() && s.back() == ' ')
            s.pop_back();
        if (s.size() < 4 || s[s.size() - 4] != '.')
            s += ".blp";
        return s;
    }

    // Where a model's flame is and what colour it burns (2026-10-01), read offline from the light doodads of
    // Stormwind, Ironforge, Undercity, Darnassus and Orgrimmar first (scratchpad m2_flames.js):
    //   Glow quads  Small blended submeshes (24 vertices or fewer, blend 2 and up): the glow a torch, a candle
    //               or a lamppost draws round its flame. A unit's colour (word 4, into the colours at 0x54: an
    //               RGB track of 28 bytes, values at +20) tints it: a Stormwind lamppost (0.92 0.74 0.22), a
    //               Darkshore one (0.33 0.74 1.00).
    //   Particles   The emitters at 0x13C, 504 bytes each in this version: the position at +8, the three
    //               colours (BGRA) a particle takes over its life at +336. Their mean, weighted by their
    //               alpha: the middle one is (223 138 47) on torches and candles, (168 107 196) on a night elf
    //               lantern, (194 0 255) on a Kalidar lamppost, and a colour at alpha 0 is never seen. One
    //               of a pole torch's emitters turns white at alpha 225, which makes its light paler.
    // The light sits at the glow quads when there are any, else at the emitters. The colour is the quads'
    // when a unit gives one, else the emitters'. Few light models carry an M2 light of their own (3 of about
    // 60, all thrones), so those are not read.
    //   Flames      An additive emitter whose texture is a flame (FLAMELICKSMALL, FLAME01, FIRE1, FIRE10020,
    //               SHAMANSTONEFLAME, the later FIRE_ ones). Of the 7150 doodad models placed in Azeroth and
    //               Kalimdor, 100 burn one and match no light word (scratchpad flame_scan.js): 44 wood piles,
    //               49 cauldrons, 27 outposts, 14 stone pyres (outside Northshire Abbey). Embers alone (a
    //               burning tree, a cart) do not count, and neither do FIRESWIRL, FIRERING and FIREPLUME.
    void Flame(const std::vector<uint8_t>& d, M2Model& out, uint32_t nSub, uint32_t ofsSub, uint32_t nUnits,
               uint32_t ofsUnits, uint32_t nMat, uint32_t ofsMat)
    {
        if (!out.pos.empty())
        {
            const size_t n = out.pos.size() / 3;
            float lo[3] = { 1e9f, 1e9f, 1e9f }, hi[3] = { -1e9f, -1e9f, -1e9f };
            for (size_t i = 0; i < n; ++i)
                for (int j = 0; j < 3; ++j)
                {
                    lo[j] = (std::min)(lo[j], out.pos[i * 3 + j]);
                    hi[j] = (std::max)(hi[j], out.pos[i * 3 + j]);
                }
            out.top[0] = (lo[0] + hi[0]) * 0.5f;
            out.top[1] = (lo[1] + hi[1]) * 0.5f;
            out.top[2] = lo[2] + (hi[2] - lo[2]) * 0.85f;
            out.height = hi[2] - lo[2];
            std::vector<float> axis(n);
            for (int j = 0; j < 3; ++j)
            {
                for (size_t i = 0; i < n; ++i)
                    axis[i] = out.pos[i * 3 + j];
                std::nth_element(axis.begin(), axis.begin() + n / 2, axis.end());
                out.middle[j] = axis[n / 2];
            }
        }
        const uint32_t nCol = U32(d, 0x54), ofsCol = U32(d, 0x58);
        float quadCol[3] = {};
        unsigned quadCols = 0;
        std::vector<bool> used(nSub, false);
        if (static_cast<uint64_t>(ofsUnits) + nUnits * 24ull <= d.size())
            for (uint32_t i = 0; i < nUnits; ++i)
            {
                const size_t o = ofsUnits + i * 24ull;
                const uint16_t sub = U16(d, o + 4), mat = U16(d, o + 10);
                const int16_t col = static_cast<int16_t>(U16(d, o + 8));
                if (sub >= nSub || used[sub] || mat >= nMat || ofsMat + mat * 4ull + 4 > d.size() ||
                    U16(d, ofsMat + mat * 4ull + 2) < 2 || ofsSub + (sub + 1) * 32ull > d.size())
                    continue;
                const uint32_t start = U16(d, ofsSub + sub * 32ull + 4), count = U16(d, ofsSub + sub * 32ull + 6);
                if (!count || count > 24 || (start + count) * 3ull > out.pos.size())
                    continue;
                used[sub] = true;
                float c[3] = {};
                for (uint32_t k = 0; k < count; ++k)
                    for (int j = 0; j < 3; ++j)
                        c[j] += out.pos[(start + k) * 3 + j] / count;
                out.quadPts.insert(out.quadPts.end(), c, c + 3);
                if (col >= 0 && static_cast<uint32_t>(col) < nCol && ofsCol + (col + 1) * 56ull <= d.size())
                {
                    const size_t t = ofsCol + col * 56ull;
                    const uint32_t nv = U32(d, t + 20), ov = U32(d, t + 24);
                    if (nv && ov + 12ull <= d.size())
                    {
                        float rgb[3];
                        memcpy(rgb, &d[ov], 12);
                        for (int j = 0; j < 3; ++j)
                            quadCol[j] += rgb[j];
                        ++quadCols;
                    }
                }
            }
        const uint32_t nPart = d.size() >= 0x144 ? U32(d, 0x13C) : 0, ofsPart = d.size() >= 0x144 ? U32(d, 0x140) : 0;
        const uint32_t nTex = U32(d, 0x5C), ofsTex = U32(d, 0x60);
        float partCol[3] = {};
        unsigned parts = 0;
        if (nPart < 64 && static_cast<uint64_t>(ofsPart) + nPart * 504ull <= d.size())
            for (uint32_t i = 0; i < nPart; ++i)
            {
                const size_t o = ofsPart + i * 504ull;
                // Additive emitters only (blend 4): a Duskwood lamppost's smoke is blend 2, and it is grey.
                if (d[o + 40] != 4)
                    continue;
                // Switched off at rest: the "enabled" track (uint8, at +476) starts at 0. The Blackrock arena
                // flag, a game object in Stormwind, burns its flames only when an animation turns them on, and
                // it gave light standing still (2026-10-01). Every lit lamp read starts at 1.
                {
                    const uint32_t keys = U32(d, o + 476 + 20), vals = U32(d, o + 476 + 24);
                    if (keys && vals < d.size() && d[vals] == 0)
                        continue;
                }
                float p[3];
                memcpy(p, &d[o + 8], 12);
                out.emitPts.insert(out.emitPts.end(), p, p + 3);
                const uint16_t tex = U16(d, o + 22);
                if (tex < nTex && ofsTex + (tex + 1) * 16ull <= d.size() && U32(d, ofsTex + tex * 16ull) == 0)
                {
                    std::string t = TextureName(d, U32(d, ofsTex + tex * 16ull + 12), U32(d, ofsTex + tex * 16ull + 8));
                    for (char& ch : t)
                        ch = ch >= 'a' && ch <= 'z' ? static_cast<char>(ch - 32) : ch;
                    t = t.substr(t.find_last_of("\\/") + 1);
                    if (t.find("FLAME") != std::string::npos || t.find("FIRE1") != std::string::npos ||
                        t.find("FIRE_") != std::string::npos)
                        out.firePts.insert(out.firePts.end(), p, p + 3);
                }
                float sum[3] = {}, weight = 0.0f;
                for (int k = 0; k < 3; ++k)
                {
                    const uint32_t c = U32(d, o + 336 + k * 4);   // BGRA
                    const float a = ((c >> 24) & 0xFF) / 255.0f;
                    sum[0] += a * ((c >> 16) & 0xFF) / 255.0f;
                    sum[1] += a * ((c >> 8) & 0xFF) / 255.0f;
                    sum[2] += a * (c & 0xFF) / 255.0f;
                    weight += a;
                }
                if (weight < 1e-3f)
                {
                    const uint32_t c = U32(d, o + 340);   // all at alpha 0: the middle one
                    sum[0] = ((c >> 16) & 0xFF) / 255.0f;
                    sum[1] = ((c >> 8) & 0xFF) / 255.0f;
                    sum[2] = (c & 0xFF) / 255.0f;
                    weight = 1.0f;
                }
                for (int j = 0; j < 3; ++j)
                    partCol[j] += sum[j] / weight;
                ++parts;
            }
        out.flame = !out.quadPts.empty() ? 2 : parts ? 1 : 0;
        const float* rgb = quadCols ? quadCol : parts ? partCol : nullptr;
        if (rgb)
        {
            const float top = (std::max)(rgb[0], (std::max)(rgb[1], rgb[2]));
            if (top > 1e-4f)
            {
                out.haveColour = true;
                for (int j = 0; j < 3; ++j)
                    out.flameColour[j] = rgb[j] / top;
            }
        }
    }
}

bool M2Load(const std::string& name, M2Model& out)
{
    out = M2Model();
    std::string file = name;
    const size_t dot = file.find_last_of('.');
    if (dot != std::string::npos)
        file = file.substr(0, dot);
    file += ".m2";   // the tiles say .mdx or .mdl; the archives hold .m2
    std::vector<uint8_t> d;
    if (!MpqRead(file.c_str(), d) || d.size() < 0x100 || U32(d, 0) != 0x3032444D)   // "MD20"
        return false;
    const uint32_t version = U32(d, 4);
    if (version != 256 && version != 257)
        return false;
    // Animated (2026-09-30): any bone whose translation, rotation or scale track has 16 keyframes or more.
    // Bones are 108 bytes in this version: key bone, flags, parent and submesh (12), then the three tracks of
    // 28 bytes (the key count at +20), then the pivot. Offline, of 160 models on two tiles: the gryphon roost
    // (48 of 53 bones, up to 464 keys), birds (267), flies, fireflies and a training dummy (20 to 25). Lamps,
    // lampposts, a chandelier and a stone pyre animate only their flame (5 to 9 keys) and stay as they are.
    {
        const uint32_t nBones = U32(d, 0x34), ofsBones = U32(d, 0x38);
        for (uint32_t i = 0; i < nBones && !out.animated && ofsBones + (i + 1) * 108ull <= d.size(); ++i)
            for (int t = 0; t < 3; ++t)
                if (U32(d, ofsBones + i * 108ull + 12 + t * 28 + 20) >= 16)
                    out.animated = true;
    }
    const uint32_t nVert = U32(d, 0x44), ofsVert = U32(d, 0x48);
    const uint32_t nViews = U32(d, 0x4C), ofsViews = U32(d, 0x50);
    const uint32_t nTex = U32(d, 0x5C), ofsTex = U32(d, 0x60);
    const uint32_t nMat = U32(d, 0x84), ofsMat = U32(d, 0x88);
    const uint32_t nLook = U32(d, 0x94), ofsLook = U32(d, 0x98);
    if (!nViews || static_cast<uint64_t>(ofsVert) + static_cast<uint64_t>(nVert) * 48 > d.size() ||
        static_cast<uint64_t>(ofsViews) + 44 > d.size())
        return false;
    const size_t v = ofsViews;
    const uint32_t nIndex = U32(d, v), ofsIndex = U32(d, v + 4);
    const uint32_t nTris = U32(d, v + 8), ofsTris = U32(d, v + 12);
    const uint32_t nSub = U32(d, v + 24), ofsSub = U32(d, v + 28);
    const uint32_t nUnits = U32(d, v + 32), ofsUnits = U32(d, v + 36);
    if (nTris % 3 || static_cast<uint64_t>(ofsIndex) + nIndex * 2ull > d.size() ||
        static_cast<uint64_t>(ofsTris) + nTris * 2ull > d.size() || nIndex > 65535)
        return false;

    out.pos.resize(static_cast<size_t>(nIndex) * 3);
    out.uv.resize(static_cast<size_t>(nIndex) * 2);
    for (uint32_t i = 0; i < nIndex; ++i)
    {
        const uint16_t g = U16(d, ofsIndex + i * 2);
        if (g >= nVert)
            return false;
        const size_t o = ofsVert + static_cast<size_t>(g) * 48;
        memcpy(&out.pos[i * 3], &d[o], 12);
        memcpy(&out.uv[i * 2], &d[o + 32], 8);
    }
    out.tris.resize(nTris);
    memcpy(out.tris.data(), &d[ofsTris], nTris * 2ull);
    for (uint16_t t : out.tris)
        if (t >= nIndex)
            return false;

    // Each submesh's first texture unit: its blend mode and its texture.
    std::vector<int> subBlend(nSub, 0), subTex(nSub, -1);
    std::vector<bool> named(nSub, false);
    if (static_cast<uint64_t>(ofsUnits) + nUnits * 24ull <= d.size())
        for (uint32_t i = 0; i < nUnits; ++i)
        {
            const size_t o = ofsUnits + i * 24ull;
            const uint16_t sub = U16(d, o + 4), mat = U16(d, o + 10), look = U16(d, o + 16);
            if (sub >= nSub || named[sub])
                continue;
            named[sub] = true;
            if (mat < nMat && ofsMat + mat * 4ull + 4 <= d.size())
                subBlend[sub] = U16(d, ofsMat + mat * 4ull + 2);
            if (look < nLook && ofsLook + look * 2ull + 2 <= d.size())
            {
                const uint16_t t = U16(d, ofsLook + look * 2ull);
                if (t < nTex)
                    subTex[sub] = t;
            }
        }
    for (uint32_t i = 0; i < nSub && ofsSub + (i + 1) * 32ull <= d.size(); ++i)
    {
        const size_t o = ofsSub + i * 32ull;
        const uint32_t start = U16(d, o + 8) + (static_cast<uint32_t>(U16(d, o + 2)) << 16);
        const uint32_t count = U16(d, o + 10);
        if (start + count > nTris)
            continue;
        M2Model::Batch b{ start, count, subBlend[i], std::string() };
        if (subTex[i] >= 0 && ofsTex + (subTex[i] + 1) * 16ull <= d.size())
        {
            const size_t t = ofsTex + subTex[i] * 16ull;
            if (U32(d, t) == 0)
                b.tex = TextureName(d, U32(d, t + 12), U32(d, t + 8));
        }
        if (b.blend == 1)
        {
            if (b.tex.empty())
                continue;   // a cut-out with no file to cut it by: it cannot cast its shape
            out.alpha = true;
        }
        out.batches.push_back(std::move(b));
    }
    if (out.batches.empty() && nTris)
        out.batches.push_back({ 0, nTris, 0, std::string() });
    Flame(d, out, nSub, ofsSub, nUnits, ofsUnits, nMat, ofsMat);
    return true;
}

bool BlpLoad(const std::string& name, BlpData& out)
{
    out = BlpData();
    std::vector<uint8_t> d;
    if (!MpqRead(name.c_str(), d) || d.size() < 148 || memcmp(d.data(), "BLP2", 4) != 0)
        return false;
    const uint8_t enc = d[8], alphaDepth = d[9], alphaEnc = d[10];
    out.width = U32(d, 12);
    out.height = U32(d, 16);
    if (!out.width || !out.height || out.width > 4096 || out.height > 4096)
        return false;
    if (enc == 2)
        out.format = alphaEnc == 7 ? 2 : alphaEnc == 1 ? 1 : 0;
    else if (enc == 1 || enc == 3)
        out.format = 3;
    else
        return false;
    for (int i = 0; i < 16; ++i)
    {
        const uint32_t ofs = U32(d, 20 + i * 4), size = U32(d, 84 + i * 4);
        const uint32_t w = (std::max)(1u, out.width >> i), h = (std::max)(1u, out.height >> i);
        if (!ofs || !size || static_cast<uint64_t>(ofs) + size > d.size())
            break;
        std::vector<uint8_t> level;
        if (enc == 2)
        {
            const uint32_t need = (std::max)(1u, (w + 3) / 4) * (std::max)(1u, (h + 3) / 4) * (out.format == 0 ? 8 : 16);
            if (size < need)
                break;
            level.assign(d.begin() + ofs, d.begin() + ofs + need);
        }
        else if (enc == 3)
        {
            if (size < w * h * 4)
                break;
            level.assign(d.begin() + ofs, d.begin() + ofs + w * h * 4);   // BGRA: A8R8G8B8's own byte order
            if (!alphaDepth)
                for (size_t p = 3; p < level.size(); p += 4)
                    level[p] = 255;
        }
        else
        {
            const uint32_t n = w * h;
            const uint32_t alphaBytes = alphaDepth == 8 ? n : alphaDepth == 4 ? (n + 1) / 2 : alphaDepth == 1 ? (n + 7) / 8 : 0;
            if (size < n + alphaBytes || d.size() < 148 + 1024)
                break;
            const uint8_t* src = &d[ofs];
            const uint8_t* pal = &d[148];
            level.resize(static_cast<size_t>(n) * 4);
            for (uint32_t p = 0; p < n; ++p)
            {
                memcpy(&level[p * 4], &pal[src[p] * 4], 3);   // the palette is BGRA too
                uint8_t a = 255;
                if (alphaDepth == 8) a = src[n + p];
                else if (alphaDepth == 4) a = static_cast<uint8_t>(((src[n + p / 2] >> ((p & 1) * 4)) & 0x0F) * 17);
                else if (alphaDepth == 1) a = ((src[n + p / 8] >> (p & 7)) & 1) ? 255 : 0;
                level[p * 4 + 3] = a;
            }
        }
        out.levels.push_back(std::move(level));
        if (w == 1 && h == 1)
            break;
    }
    return !out.levels.empty();
}
