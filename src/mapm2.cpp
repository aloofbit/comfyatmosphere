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
