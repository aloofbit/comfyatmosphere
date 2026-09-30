// lamps: a probe for local lights (street lamps, lanterns, torches), to find where their positions can
// be taken from before any light is drawn for them. F12 opens it with the frame probe. Stand still: the
// report places each light by where it was first seen, and a moving camera spreads one light into several.
//
// Three sources are watched over a window of kWindow frames, in the world phase only (after the sky,
// before the world ends):
//
//   Shader lights  The client's M2 vertex shaders light a model with up to two point lights. From the
//                  disassembly of shader 1229F540 (comfyfog.log, 2026-09-28):
//                      add r4.xyz, -r0, c21          vertex -> first light
//                      add r3.xyz, -r0, c22          vertex -> second light
//                      mad r3.xy, r6, c27, r7        d^2 * c27 + d * c26
//                      add r3.xy, r3, c25            ... + c25
//                      mad r1.xyz, c17, r2.x, r1     first light's colour * N.L / attenuation
//                      mad r1.xyz, c18, r2.y, r1     second light's colour
//                  r0 is the skinned vertex, in the space c2..c5 takes to clip. So a light's position goes
//                  through c2..c5 to clip and back through the world camera to camera-relative world, the
//                  same way for models that fold their world matrix into c2..c5 and for those that carry
//                  it in the bones (c2..c5 then holds the projection alone and r0 is in view space).
//                  Attenuation is 1 / (c25 + c26 d + c27 d^2), .x for the first light, .y for the second.
//   Fixed lights   SetLight and LightEnable, and the point and spot lights enabled at each fixed-function
//                  draw with LIGHTING on. comfygrass only ever read the directional one.
//   Glows          Additive draws (DESTBLEND ONE). A lamp's glow sprite is one; so are spell effects and
//                  particles. Each is placed by the centre of its first vertices and fingerprinted by its
//                  texture: size, format and the colour of its smallest mip level. The client fogs these
//                  to black (see OutColor in comfyfog.cpp), which the report counts as a check. A Darkshire
//                  lamppost's glow has a grey texture (probe, 2026-09-28), so its warm colour comes from
//                  elsewhere: the unlit shaders draw c28 + c29 (mov r0, c28; add oD0, r0, c29), which is
//                  logged as the tint.
//
// Each source becomes a list of places in absolute world coordinates, merged within kMerge yards, with how
// many frames of the window each was seen in. A lamp is in all or nearly all of them; a spell effect that
// moves is not. The first kRawLines new places of each source are also logged as they are found, and the
// SetLight calls of the first frame.
//
// The tracker (below the probe) keeps the lights the glow draws (lampglow.cpp), every frame the glow is on.
// It takes two of the three sources, as the probe found them in Darkshire (NOTES.md):
//
//   Client lights  Point lights from SetLight: torches and braziers, carried by NPCs too. The position is
//                  camera-relative; the colour and the attenuation are the client's.
//   Lamp sprites   Four-vertex additive M2 draws with a warm c28 + c29: the lampposts, which have no
//                  light of their own. A sprite must stay where it was first seen: one that moves more
//                  than kStill yards is a spell effect and never glows. It glows only after kSpriteDelay
//                  seconds, so a brief spark does not either.
//
// Sightings are merged into lights held in world coordinates. A light is matched to the nearest held light
// of its kind within kFollow yards, so an NPC's torch moves with the NPC. The client names a light only
// while it draws a model near it, and draws a sprite only while its lamp is on screen, so a light off
// screen is kept as it is: its glow can still reach the air in view (Darkshire, 2026-09-28: a lamppost
// beside the camera faded out, and the fog around it went dark). Only the time a light spends on screen
// unseen counts; after [lamps] keep seconds of it the light is dropped, fading out after kGrace. A light
// further than kForget yards past [lamps] maxDistance is dropped too.

#define CINTERFACE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <d3d9.h>

#include "client.h"
#include "mapterrain.h"
#include "common.h"
#include "config.h"
#include "lamps.h"
#include "shadow.h"
#include "sun.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    constexpr unsigned kWindow        = 60;    // frames the probe gathers over
    constexpr float    kMerge         = 1.0f;  // yards: two sightings this near are one place
    constexpr size_t   kMaxPlaces     = 512;   // per source
    constexpr unsigned kRawLines      = 40;    // per source: sightings logged as they happen
    constexpr unsigned kReportLines   = 40;    // per source: places in the report, nearest first
    constexpr unsigned kSample        = 32;    // vertices read from a glow draw
    constexpr unsigned kReadsPerFrame = 400;   // glow draws read per frame; the rest are counted only

    // The tracker.
    constexpr unsigned kTrackReads    = 32;    // sprite draws read per frame
    constexpr size_t   kMaxSightings  = 128;   // per frame
    constexpr size_t   kMaxTracked    = 256;
    constexpr float    kSame          = 0.2f;  // yards: two sightings in one frame this near are one light
    constexpr float    kFollow[2]     = { 3.0f, 1.0f };   // yards a light may move between frames, by kind
    constexpr float    kStill         = 1.0f;  // yards a sprite may drift from where it was first seen
    constexpr float    kNearLight     = 1.5f;  // yards: a sprite this near a client light is that light
    constexpr double   kGrace         = 0.25;  // seconds unseen before a light starts to fade
    constexpr float    kEdge          = 0.03f; // share of the screen at each edge that does not count as on it
    constexpr float    kForget        = 60.0f; // yards past [lamps] maxDistance at which a held light goes
    constexpr double   kFadeIn[2]     = { 0.2, 0.35 };    // seconds, by kind
    constexpr double   kSpriteDelay   = 0.25;  // seconds a sprite must be seen before it glows
    constexpr unsigned kCacheFrames   = 600;   // the shader and declaration caches are dropped this often,
                                               // so a freed object's address cannot be taken for a new one

    // ---------------------------------------------------------------------------------------------
    // matrices (D3D9: row vectors, v' = v * M)

    void Mul(const D3DMATRIX& a, const D3DMATRIX& b, D3DMATRIX& out)
    {
        D3DMATRIX r;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j] + a.m[i][3] * b.m[3][j];
        out = r;
    }

    // General 4x4 inverse, in double: the camera's view-projection has a near plane of 0.1 yards.
    bool Invert(const D3DMATRIX& src, D3DMATRIX& out)
    {
        double a[4][8];
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 8; ++j)
                a[i][j] = j < 4 ? src.m[i][j] : (j - 4 == i ? 1.0 : 0.0);
        for (int c = 0; c < 4; ++c)
        {
            int p = c;
            for (int r = c + 1; r < 4; ++r)
                if (fabs(a[r][c]) > fabs(a[p][c])) p = r;
            if (fabs(a[p][c]) < 1e-12)
                return false;
            if (p != c)
                for (int j = 0; j < 8; ++j) { const double t = a[c][j]; a[c][j] = a[p][j]; a[p][j] = t; }
            const double inv = 1.0 / a[c][c];
            for (int j = 0; j < 8; ++j) a[c][j] *= inv;
            for (int r = 0; r < 4; ++r)
                if (r != c && a[r][c] != 0.0)
                {
                    const double f = a[r][c];
                    for (int j = 0; j < 8; ++j) a[r][j] -= f * a[c][j];
                }
        }
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                out.m[i][j] = static_cast<float>(a[i][j + 4]);
        return true;
    }

    void Mul4(const float in[4], const D3DMATRIX& m, float out[4])
    {
        for (int c = 0; c < 4; ++c)
            out[c] = in[0] * m.m[0][c] + in[1] * m.m[1][c] + in[2] * m.m[2][c] + in[3] * m.m[3][c];
    }

    // c2..c5 as the shader reads them (each register one column) -> the matrix.
    void FromRegisters(const float* c, D3DMATRIX& m)
    {
        for (int col = 0; col < 4; ++col)
            for (int row = 0; row < 4; ++row)
                m.m[row][col] = c[col * 4 + row];
    }

    float Len3(const float v[3]) { return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

    // ---------------------------------------------------------------------------------------------
    // state

    struct Place
    {
        float       abs[3]    = {};    // absolute world position, as first seen
        float       raw[3]    = {};    // fixed lights: the position as the client set it
        float       dist      = 0.0f;  // yards from the camera, as first seen
        float       uv[2]     = { -1.0f, -1.0f };   // on screen, as first seen; -1 if behind the camera
        float       colour[3] = {};    // the brightest seen
        float       att[3]    = {};    // constant, linear, quadratic
        float       range     = 0.0f;  // fixed lights: D3DLIGHT9 Range
        float       size      = 0.0f;  // glows: how far the read vertices reach from their centre
        float       tint[4]   = {};    // glows through a shader: c28 + c29, the colour an unlit shader draws
        unsigned    draws     = 0;
        unsigned    frames    = 0;
        unsigned    lastFrame = 0;     // window frame + 1 it was last seen in
        unsigned    slots     = 0;     // shader lights: bit 0 = seen as the first light, bit 1 = the second
        unsigned    blackFog  = 0;     // glows: draws fogged to black
        unsigned    verts     = 0;     // glows: vertices in the first draw
        const void* tex       = nullptr;
        DWORD       kind      = 0;     // fixed: the light type; glows: SRCBLEND
        bool        shader    = false; // glows: drawn through a vertex shader
    };

    struct PlaceList
    {
        std::vector<Place> places;
        unsigned           overflow = 0;   // sightings with no room left
    };

    struct ShaderInfo
    {
        bool light[2] = {};    // reads c21 / c22
        bool bones    = false; // skins through c31[a0.?]
        char biComp   = 0;     // the blend-index component the shader reads first: 'x'..'w'
    };

    struct DeclInfo
    {
        bool ok       = false;   // a position we can read (not pre-transformed)
        UINT posOff   = 0;
        bool bi       = false;
        UINT biOff    = 0;
        BYTE biType   = 0;
    };

    struct TexInfo
    {
        UINT      w = 0, h = 0, levels = 0;
        D3DFORMAT fmt = D3DFMT_UNKNOWN;
        bool      haveColour = false;
        float     colour[3] = {};
    };

    bool     g_active = false;
    unsigned g_windowFrame = 0;   // 0 .. kWindow-1
    uint64_t g_probeNumber = 0;
    float    g_c[256 * 4];        // the vertex-shader constants as the client last set them

    // Per frame, filled at the first use.
    bool      g_frameReady = false;
    bool      g_haveCam = false, g_haveVP = false;
    float     g_cam[3] = {};
    D3DMATRIX g_vp = {}, g_vpInv = {};
    unsigned  g_reads = 0;

    // Counts over the window.
    unsigned g_nDraws = 0, g_nLightable = 0, g_nLit = 0, g_nShaderUnplaced = 0;
    unsigned g_nFfLit = 0, g_nFfLocal = 0;
    unsigned g_nSetLight = 0, g_nEnableOn[9] = {}, g_nEnableOff[9] = {};
    unsigned g_nAdditive = 0, g_nUnread = 0, g_nShaderAdditive = 0;
    unsigned g_rawShader = 0, g_rawFixed = 0, g_rawGlow = 0;

    PlaceList g_shaderPlaces, g_fixedPlaces, g_glowPlaces;
    std::unordered_map<const void*, ShaderInfo> g_shaders;
    std::unordered_map<const void*, DeclInfo>   g_decls;
    std::unordered_map<const void*, TexInfo>    g_texs;

    void ClearWindow()
    {
        g_windowFrame = 0;
        g_frameReady = false;
        g_nDraws = g_nLightable = g_nLit = g_nShaderUnplaced = 0;
        g_nFfLit = g_nFfLocal = 0;
        g_nSetLight = 0;
        memset(g_nEnableOn, 0, sizeof(g_nEnableOn));
        memset(g_nEnableOff, 0, sizeof(g_nEnableOff));
        g_nAdditive = g_nUnread = g_nShaderAdditive = 0;
        g_rawShader = g_rawFixed = g_rawGlow = 0;
        g_shaderPlaces = PlaceList();
        g_fixedPlaces  = PlaceList();
        g_glowPlaces   = PlaceList();
        g_shaders.clear();
        g_decls.clear();
        g_texs.clear();
    }

    void EnsureFrame()
    {
        if (g_frameReady)
            return;
        g_frameReady = true;
        g_reads = 0;
        g_haveCam = ClientCamera(g_cam);
        D3DMATRIX view, proj;
        g_haveVP = SunCamera(view, proj);
        // The projection the world is drawn with, when the light's vote has one: the latest perspective
        // projection can be the far horizon's, whose near and far planes differ. The view is this frame's
        // either way; the vote's is a frame old, and a turn would move every sprite.
        D3DMATRIX wv, wp;
        if (g_haveVP && ShadowWorldCamera(wv, wp))
            proj = wp;
        if (g_haveVP)
        {
            Mul(view, proj, g_vp);
            g_haveVP = Invert(g_vp, g_vpInv);
        }
    }

    // Clip space -> camera-relative world, through the world camera.
    bool ClipToCamRel(const float clip[4], float out[3])
    {
        if (!g_haveVP)
            return false;
        float h[4];
        Mul4(clip, g_vpInv, h);
        if (!(fabsf(h[3]) > 1e-9f))
            return false;
        for (int i = 0; i < 3; ++i)
            out[i] = h[i] / h[3];
        return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
    }

    void ScreenUV(const float camRel[3], float uv[2])
    {
        uv[0] = uv[1] = -1.0f;
        if (!g_haveVP)
            return;
        const float p[4] = { camRel[0], camRel[1], camRel[2], 1.0f };
        float c[4];
        Mul4(p, g_vp, c);
        if (c[3] <= 1e-5f)
            return;
        uv[0] = c[0] / c[3] * 0.5f + 0.5f;
        uv[1] = -c[1] / c[3] * 0.5f + 0.5f;
    }

    // A place for this sighting: an existing one within kMerge yards (and with the same texture and kind),
    // or a new one. Null when the list is full.
    Place* Sight(PlaceList& list, const float camRel[3], const void* tex, DWORD kind, bool* isNew)
    {
        float abs[3];
        for (int i = 0; i < 3; ++i)
            abs[i] = camRel[i] + (g_haveCam ? g_cam[i] : 0.0f);
        *isNew = false;
        Place* p = nullptr;
        for (Place& q : list.places)
        {
            if (q.tex != tex || q.kind != kind)
                continue;
            const float d[3] = { q.abs[0] - abs[0], q.abs[1] - abs[1], q.abs[2] - abs[2] };
            if (Len3(d) <= kMerge) { p = &q; break; }
        }
        if (!p)
        {
            if (list.places.size() >= kMaxPlaces)
            {
                ++list.overflow;
                return nullptr;
            }
            list.places.emplace_back();
            p = &list.places.back();
            memcpy(p->abs, abs, sizeof(abs));
            p->dist = Len3(camRel);
            ScreenUV(camRel, p->uv);
            p->tex  = tex;
            p->kind = kind;
            *isNew = true;
        }
        ++p->draws;
        if (p->lastFrame != g_windowFrame + 1)
        {
            p->lastFrame = g_windowFrame + 1;
            ++p->frames;
        }
        return p;
    }

    void KeepBrightest(Place& p, const float rgb[3])
    {
        if (rgb[0] + rgb[1] + rgb[2] > p.colour[0] + p.colour[1] + p.colour[2])
            memcpy(p.colour, rgb, sizeof(p.colour));
    }

    // Where 1 / (c + l d + q d^2) falls to 5%, in yards; 0 if it never does.
    float Reach(const float att[3])
    {
        const float c = att[0] - 20.0f, l = att[1], q = att[2];
        if (q > 1e-9f)
        {
            const float disc = l * l - 4.0f * q * c;
            return disc >= 0.0f ? (-l + sqrtf(disc)) / (2.0f * q) : 0.0f;
        }
        return l > 1e-9f ? -c / l : 0.0f;
    }

    // ---------------------------------------------------------------------------------------------
    // what a shader reads

    // Does this line read register cN? Definitions, declarations and comments do not count.
    bool LineReads(const std::string& line, int n)
    {
        size_t s = line.find_first_not_of(" \t");
        if (s == std::string::npos || line.compare(s, 2, "//") == 0 || line.compare(s, 3, "def") == 0 ||
            line.compare(s, 3, "dcl") == 0)
            return false;
        char want[8];
        _snprintf_s(want, sizeof(want), _TRUNCATE, "c%d", n);
        const size_t wl = strlen(want);
        for (size_t at = line.find(want, s); at != std::string::npos; at = line.find(want, at + 1))
        {
            const char before = at ? line[at - 1] : ' ';
            const char after  = at + wl < line.size() ? line[at + wl] : ' ';
            if (!isalnum(static_cast<unsigned char>(before)) && before != '_' && !isdigit(static_cast<unsigned char>(after)))
                return true;
        }
        return false;
    }

    const ShaderInfo& Analyse(IDirect3DVertexShader9* sh)
    {
        auto it = g_shaders.find(sh);
        if (it != g_shaders.end())
            return it->second;
        ShaderInfo& info = g_shaders[sh];

        static auto disasm = reinterpret_cast<PFN_D3DDisassemble>(CompilerProc("D3DDisassemble"));
        UINT size = 0;
        if (!disasm || FAILED(sh->lpVtbl->GetFunction(sh, nullptr, &size)) || !size)
            return info;
        std::vector<uint8_t> code(size);
        if (FAILED(sh->lpVtbl->GetFunction(sh, code.data(), &size)))
            return info;
        OgBlob* blob = nullptr;
        if (FAILED(disasm(code.data(), size, 0, nullptr, &blob)) || !blob)
            return info;
        const std::string text(static_cast<const char*>(blob->lpVtbl->GetBufferPointer(blob)));
        blob->lpVtbl->Release(blob);

        info.bones = text.find("c31[") != std::string::npos;
        std::string biReg;
        size_t pos = 0;
        while (pos < text.size())
        {
            size_t end = text.find('\n', pos);
            if (end == std::string::npos) end = text.size();
            const std::string line = text.substr(pos, end - pos);
            pos = end + 1;
            if (LineReads(line, 21)) info.light[0] = true;
            if (LineReads(line, 22)) info.light[1] = true;
            const size_t d = line.find("dcl_blendindices ");
            if (d != std::string::npos)
            {
                biReg = line.substr(d + 17);
                biReg.erase(biReg.find_last_not_of(" \t\r") + 1);
                continue;
            }
            // The first component of the blend-index register read after its declaration: "v2.z" and
            // "v2.zyxw" both say z.
            if (!biReg.empty() && !info.biComp)
            {
                const size_t r = line.find(biReg + ".");
                if (r != std::string::npos && r + biReg.size() + 1 < line.size())
                    info.biComp = line[r + biReg.size() + 1];
            }
        }
        if (g_active)
            Log("lamps: shader %p reads light 1 %s, light 2 %s, bones %s, blend index .%c",
                sh, info.light[0] ? "yes" : "no", info.light[1] ? "yes" : "no", info.bones ? "yes" : "no",
                info.biComp ? info.biComp : '-');
        return info;
    }

    const DeclInfo& Decl(IDirect3DDevice9* dev)
    {
        IDirect3DVertexDeclaration9* decl = nullptr;
        dev->lpVtbl->GetVertexDeclaration(dev, &decl);
        if (!decl)
        {
            DWORD fvf = 0;
            dev->lpVtbl->GetFVF(dev, &fvf);
            static DeclInfo fvfInfo;
            fvfInfo = DeclInfo();
            const DWORD p = fvf & D3DFVF_POSITION_MASK;
            fvfInfo.ok = p != 0 && p != D3DFVF_XYZRHW;
            return fvfInfo;
        }
        auto it = g_decls.find(decl);
        if (it != g_decls.end())
        {
            decl->lpVtbl->Release(decl);
            return it->second;
        }
        DeclInfo& info = g_decls[decl];
        UINT n = 0;
        if (SUCCEEDED(decl->lpVtbl->GetDeclaration(decl, nullptr, &n)) && n)
        {
            std::vector<D3DVERTEXELEMENT9> el(n);
            if (SUCCEEDED(decl->lpVtbl->GetDeclaration(decl, el.data(), &n)))
                for (const D3DVERTEXELEMENT9& e : el)
                {
                    if (e.Stream != 0 || e.Type == D3DDECLTYPE_UNUSED)
                        continue;
                    if (e.Usage == D3DDECLUSAGE_POSITION && e.UsageIndex == 0 &&
                        (e.Type == D3DDECLTYPE_FLOAT3 || e.Type == D3DDECLTYPE_FLOAT4))
                    {
                        info.ok = true;
                        info.posOff = e.Offset;
                    }
                    if (e.Usage == D3DDECLUSAGE_BLENDINDICES && e.UsageIndex == 0)
                    {
                        info.bi = true;
                        info.biOff = e.Offset;
                        info.biType = e.Type;
                    }
                }
        }
        decl->lpVtbl->Release(decl);
        return info;
    }

    // ---------------------------------------------------------------------------------------------
    // textures

    const char* FormatName(D3DFORMAT f)
    {
        switch (f)
        {
        case D3DFMT_DXT1:     return "DXT1";
        case D3DFMT_DXT2:     return "DXT2";
        case D3DFMT_DXT3:     return "DXT3";
        case D3DFMT_DXT4:     return "DXT4";
        case D3DFMT_DXT5:     return "DXT5";
        case D3DFMT_A8R8G8B8: return "A8R8G8B8";
        case D3DFMT_X8R8G8B8: return "X8R8G8B8";
        case D3DFMT_A4R4G4B4: return "A4R4G4B4";
        case D3DFMT_A1R5G5B5: return "A1R5G5B5";
        case D3DFMT_X1R5G5B5: return "X1R5G5B5";
        case D3DFMT_R5G6B5:   return "R5G6B5";
        default:              return "other";
        }
    }

    void Rgb565(uint16_t c, float out[3])
    {
        out[0] = ((c >> 11) & 31) / 31.0f;
        out[1] = ((c >>  5) & 63) / 63.0f;
        out[2] = ( c        & 31) / 31.0f;
    }

    // Size, format and the average colour of the smallest mip level: a fingerprint that tells a warm lamp
    // glow from a blue spell effect without the texture's name.
    const TexInfo& Texture(IDirect3DBaseTexture9* base)
    {
        auto it = g_texs.find(base);
        if (it != g_texs.end())
            return it->second;
        TexInfo& info = g_texs[base];
        if (base->lpVtbl->GetType(base) != D3DRTYPE_TEXTURE)
            return info;
        auto* t = reinterpret_cast<IDirect3DTexture9*>(base);
        D3DSURFACE_DESC d = {};
        if (FAILED(t->lpVtbl->GetLevelDesc(t, 0, &d)))
            return info;
        info.w = d.Width; info.h = d.Height; info.fmt = d.Format;
        info.levels = t->lpVtbl->GetLevelCount(t);
        const UINT lv = info.levels ? info.levels - 1 : 0;
        if (FAILED(t->lpVtbl->GetLevelDesc(t, lv, &d)))
            return info;
        D3DLOCKED_RECT lr = {};
        if (FAILED(t->lpVtbl->LockRect(t, lv, &lr, nullptr, D3DLOCK_READONLY)) || !lr.pBits)
            return info;   // a default-pool texture: no colour, the size still helps

        const uint8_t* bits = static_cast<const uint8_t*>(lr.pBits);
        float sum[3] = {};
        unsigned n = 0;
        if (d.Format == D3DFMT_DXT1 || d.Format == D3DFMT_DXT2 || d.Format == D3DFMT_DXT3 ||
            d.Format == D3DFMT_DXT4 || d.Format == D3DFMT_DXT5)
        {
            // The first block's two end colours; the block is the whole of a mip level this small.
            const uint8_t* blk = bits + (d.Format == D3DFMT_DXT1 ? 0 : 8);
            uint16_t c0, c1;
            memcpy(&c0, blk, 2);
            memcpy(&c1, blk + 2, 2);
            float a[3], b[3];
            Rgb565(c0, a);
            Rgb565(c1, b);
            for (int i = 0; i < 3; ++i) sum[i] = a[i] + b[i];
            n = 2;
        }
        else
        {
            const UINT w = std::min<UINT>(d.Width, 4), h = std::min<UINT>(d.Height, 4);
            for (UINT y = 0; y < h; ++y)
                for (UINT x = 0; x < w; ++x)
                {
                    const uint8_t* px = bits + y * lr.Pitch;
                    float c[3];
                    if (d.Format == D3DFMT_A8R8G8B8 || d.Format == D3DFMT_X8R8G8B8)
                    {
                        uint32_t v; memcpy(&v, px + 4 * x, 4);
                        c[0] = ((v >> 16) & 255) / 255.0f; c[1] = ((v >> 8) & 255) / 255.0f; c[2] = (v & 255) / 255.0f;
                    }
                    else if (d.Format == D3DFMT_A4R4G4B4)
                    {
                        uint16_t v; memcpy(&v, px + 2 * x, 2);
                        c[0] = ((v >> 8) & 15) / 15.0f; c[1] = ((v >> 4) & 15) / 15.0f; c[2] = (v & 15) / 15.0f;
                    }
                    else if (d.Format == D3DFMT_A1R5G5B5 || d.Format == D3DFMT_X1R5G5B5)
                    {
                        uint16_t v; memcpy(&v, px + 2 * x, 2);
                        c[0] = ((v >> 10) & 31) / 31.0f; c[1] = ((v >> 5) & 31) / 31.0f; c[2] = (v & 31) / 31.0f;
                    }
                    else if (d.Format == D3DFMT_R5G6B5)
                    {
                        uint16_t v; memcpy(&v, px + 2 * x, 2);
                        Rgb565(v, c);
                    }
                    else
                    {
                        continue;
                    }
                    for (int i = 0; i < 3; ++i) sum[i] += c[i];
                    ++n;
                }
        }
        t->lpVtbl->UnlockRect(t, lv);
        if (n)
        {
            info.haveColour = true;
            for (int i = 0; i < 3; ++i) info.colour[i] = sum[i] / n;
        }
        return info;
    }

    // ---------------------------------------------------------------------------------------------
    // the three sources

    // c2..c5 as a matrix, and a point in its input space -> camera-relative world.
    bool ShaderToCamRel(const float p[3], float out[3])
    {
        D3DMATRIX m;
        FromRegisters(&g_c[2 * 4], m);
        const float in[4] = { p[0], p[1], p[2], 1.0f };
        float clip[4];
        Mul4(in, m, clip);
        return ClipToCamRel(clip, out);
    }

    void ShaderLights(IDirect3DVertexShader9* vs, const ShaderInfo& info)
    {
        ++g_nLightable;
        bool lit = false;
        for (int k = 0; k < 2; ++k)
        {
            if (!info.light[k])
                continue;
            const float* col = &g_c[(17 + k) * 4];
            if (std::max(col[0], std::max(col[1], col[2])) < 0.002f)
                continue;
            lit = true;
            const float* lp = &g_c[(21 + k) * 4];
            float camRel[3];
            if (!ShaderToCamRel(lp, camRel))
            {
                ++g_nShaderUnplaced;
                continue;
            }
            const float att[3] = { g_c[25 * 4 + k], g_c[26 * 4 + k], g_c[27 * 4 + k] };
            bool isNew = false;
            Place* p = Sight(g_shaderPlaces, camRel, nullptr, 0, &isNew);
            if (p)
            {
                KeepBrightest(*p, col);
                if (isNew) memcpy(p->att, att, sizeof(att));
                p->slots |= 1u << k;
            }
            if (g_rawShader < kRawLines)
            {
                ++g_rawShader;
                // The model's own key point (its first bone's origin), to see the light lands near it.
                const float key[3] = { g_c[31 * 4 + 3], g_c[32 * 4 + 3], g_c[33 * 4 + 3] };
                float model[3] = {};
                const bool haveModel = ShaderToCamRel(key, model);
                const float gap[3] = { camRel[0] - model[0], camRel[1] - model[1], camRel[2] - model[2] };
                float uv[2];
                ScreenUV(camRel, uv);
                Log("lamps: raw shader light %d, vs %p, frame %u: c%d = (%.3f %.3f %.3f) -> camera-relative "
                    "(%.2f %.2f %.2f), %.1f yd, screen (%.3f %.3f); colour (%.3f %.3f %.3f), attenuation "
                    "(%.4f %.4f %.5f), reach %.1f yd; model key point %.1f yd away, light %.1f yd from it",
                    k + 1, vs, g_windowFrame, 21 + k, lp[0], lp[1], lp[2], camRel[0], camRel[1], camRel[2],
                    Len3(camRel), uv[0], uv[1], col[0], col[1], col[2], att[0], att[1], att[2], Reach(att),
                    haveModel ? Len3(model) : -1.0f, haveModel ? Len3(gap) : -1.0f);
            }
        }
        if (lit)
            ++g_nLit;
    }

    // A point or spot light enabled at a fixed-function draw. Positions are taken as camera-relative,
    // like the world matrices; the report prints the raw value too, in case they are not.
    void FixedLight(const D3DLIGHT9& L, bool fromDraw)
    {
        if (L.Type != D3DLIGHT_POINT && L.Type != D3DLIGHT_SPOT)
            return;
        const float raw[3] = { L.Position.x, L.Position.y, L.Position.z };
        bool isNew = false;
        Place* p = Sight(g_fixedPlaces, raw, nullptr, L.Type, &isNew);
        if (!p)
            return;
        const float col[3] = { L.Diffuse.r, L.Diffuse.g, L.Diffuse.b };
        KeepBrightest(*p, col);
        if (isNew)
        {
            memcpy(p->raw, raw, sizeof(raw));
            p->att[0] = L.Attenuation0; p->att[1] = L.Attenuation1; p->att[2] = L.Attenuation2;
            p->range = L.Range;
        }
        if (!fromDraw)
            --p->draws;   // a SetLight call places it but is not a draw it lit
    }

    void FixedDraw(IDirect3DDevice9* dev)
    {
        DWORD lighting = 0;
        dev->lpVtbl->GetRenderState(dev, D3DRS_LIGHTING, &lighting);
        if (!lighting)
            return;
        ++g_nFfLit;
        bool local = false;
        for (DWORD li = 0; li < 8; ++li)
        {
            BOOL on = FALSE;
            if (FAILED(dev->lpVtbl->GetLightEnable(dev, li, &on)) || !on)
                continue;
            D3DLIGHT9 L = {};
            if (FAILED(dev->lpVtbl->GetLight(dev, li, &L)) || L.Type == D3DLIGHT_DIRECTIONAL)
                continue;
            local = true;
            FixedLight(L, true);
        }
        if (local)
            ++g_nFfLocal;
    }

    // Reads up to kSample vertices of the draw and returns their centre and reach, camera-relative.
    bool GlowCentre(IDirect3DDevice9* dev, const LampDraw& d, const ShaderInfo* sh, float centre[3], float& size)
    {
        const DeclInfo& decl = Decl(dev);
        if (!decl.ok || !d.nv)
            return false;
        const UINT n = std::min<UINT>(d.nv, kSample);

        IDirect3DVertexBuffer9* vb = nullptr;
        UINT stride = d.upStride;
        const uint8_t* base = static_cast<const uint8_t*>(d.up);
        void* locked = nullptr;
        if (!base)
        {
            UINT off = 0;
            if (FAILED(dev->lpVtbl->GetStreamSource(dev, 0, &vb, &off, &stride)) || !vb || stride < 12)
            {
                if (vb) vb->lpVtbl->Release(vb);
                return false;
            }
            if (FAILED(vb->lpVtbl->Lock(vb, off + d.first * stride, n * stride, &locked, D3DLOCK_READONLY)) || !locked)
            {
                vb->lpVtbl->Release(vb);
                return false;
            }
            base = static_cast<const uint8_t*>(locked);
        }
        else
        {
            base += d.first * stride;
        }

        std::vector<float> pts;
        pts.reserve(n * 3);
        for (UINT i = 0; i < n; ++i)
        {
            const uint8_t* v = base + i * stride;
            float p[4] = { 0, 0, 0, 1 };
            memcpy(p, v + decl.posOff, 12);
            float out[3];
            if (!sh)
            {
                float w[4];
                Mul4(p, *d.world, w);
                out[0] = w[0]; out[1] = w[1]; out[2] = w[2];
            }
            else
            {
                float r0[3] = { p[0], p[1], p[2] };
                if (sh->bones)
                {
                    // One bone, the first the vertex names; the shader's weights are left out.
                    UINT reg = 31;
                    if (decl.bi && sh->biComp)
                    {
                        const int comp = sh->biComp == 'x' ? 0 : sh->biComp == 'y' ? 1 : sh->biComp == 'z' ? 2 : 3;
                        // D3DCOLOR stores B, G, R, A, so its x is byte 2; UBYTE4 stores x first.
                        static const int colourByte[4] = { 2, 1, 0, 3 };
                        const int byte = decl.biType == D3DDECLTYPE_D3DCOLOR ? colourByte[comp] : comp;
                        reg = 31 + 3u * v[decl.biOff + byte];
                        if (reg + 2 >= 256)
                            reg = 31;
                    }
                    for (int j = 0; j < 3; ++j)
                    {
                        const float* b = &g_c[(reg + j) * 4];
                        r0[j] = b[0] * p[0] + b[1] * p[1] + b[2] * p[2] + b[3];
                    }
                }
                if (!ShaderToCamRel(r0, out))
                    continue;
            }
            pts.insert(pts.end(), out, out + 3);
        }
        if (locked)
            vb->lpVtbl->Unlock(vb);
        if (vb)
            vb->lpVtbl->Release(vb);

        const size_t m = pts.size() / 3;
        if (!m)
            return false;
        centre[0] = centre[1] = centre[2] = 0.0f;
        for (size_t i = 0; i < m; ++i)
            for (int j = 0; j < 3; ++j)
                centre[j] += pts[i * 3 + j] / m;
        size = 0.0f;
        for (size_t i = 0; i < m; ++i)
        {
            const float dv[3] = { pts[i * 3] - centre[0], pts[i * 3 + 1] - centre[1], pts[i * 3 + 2] - centre[2] };
            size = std::max(size, Len3(dv));
        }
        return true;
    }

    void Glow(IDirect3DDevice9* dev, const LampDraw& d, const ShaderInfo* sh)
    {
        DWORD blend = 0, src = 0, dst = 0;
        dev->lpVtbl->GetRenderState(dev, D3DRS_ALPHABLENDENABLE, &blend);
        if (!blend)
            return;
        dev->lpVtbl->GetRenderState(dev, D3DRS_DESTBLEND, &dst);
        if (dst != D3DBLEND_ONE)
            return;
        dev->lpVtbl->GetRenderState(dev, D3DRS_SRCBLEND, &src);
        ++g_nAdditive;
        if (sh)
            ++g_nShaderAdditive;
        if (g_reads >= kReadsPerFrame)
        {
            ++g_nUnread;
            return;
        }
        ++g_reads;

        float centre[3], size = 0.0f;
        if (!GlowCentre(dev, d, sh, centre, size))
        {
            ++g_nUnread;
            return;
        }
        IDirect3DBaseTexture9* tex = nullptr;
        dev->lpVtbl->GetTexture(dev, 0, &tex);
        const TexInfo ti = tex ? Texture(tex) : TexInfo();
        if (tex)
            tex->lpVtbl->Release(tex);   // kept only as a key

        bool isNew = false;
        Place* p = Sight(g_glowPlaces, centre, tex, src, &isNew);
        if (p)
        {
            if (isNew)
            {
                p->size   = size;
                p->verts  = d.nv;
                p->shader = sh != nullptr;
                if (ti.haveColour)
                    memcpy(p->colour, ti.colour, sizeof(p->colour));
                if (sh)
                    for (int i = 0; i < 4; ++i)
                        p->tint[i] = g_c[28 * 4 + i] + g_c[29 * 4 + i];
            }
            if ((d.fogColor & 0xFFFFFF) == 0)
                ++p->blackFog;
        }
        if (isNew && g_rawGlow < kRawLines)
        {
            ++g_rawGlow;
            float uv[2];
            ScreenUV(centre, uv);
            const float* c28 = &g_c[28 * 4];
            const float* c29 = &g_c[29 * 4];
            char tint[160] = "";
            if (sh)
                _snprintf_s(tint, sizeof(tint), _TRUNCATE, "; c28 (%.2f %.2f %.2f %.2f) c29 (%.2f %.2f %.2f %.2f)",
                            c28[0], c28[1], c28[2], c28[3], c29[0], c29[1], c29[2], c29[3]);
            Log("lamps: raw glow %s, frame %u: %u verts, centre camera-relative (%.2f %.2f %.2f), %.1f yd, "
                "screen (%.3f %.3f), size %.2f yd; tex %p %ux%u %s, %u levels, colour (%.2f %.2f %.2f)%s; "
                "src blend %u, fog 0x%06X%s",
                sh ? "M2" : "ff", g_windowFrame, d.nv, centre[0], centre[1], centre[2], Len3(centre), uv[0],
                uv[1], size, tex, ti.w, ti.h, FormatName(ti.fmt), ti.levels, ti.colour[0], ti.colour[1],
                ti.colour[2], ti.haveColour ? "" : " (not readable)", src, d.fogColor & 0xFFFFFF, tint);
        }
    }

    // ---------------------------------------------------------------------------------------------
    // the tracker

    struct Sighting
    {
        float abs[3];
        float colour[3];
        float reach;
        int   kind;
    };

    struct Tracked
    {
        float  abs[3];
        float  origin[3];      // where it was first seen
        float  colour[3];
        float  reach;
        int    kind;
        double born;
        double lastSeen;
        double unseen;         // seconds on screen and not seen, since it was last seen
        bool   mobile;         // a sprite that moved: a spell effect, never drawn
        bool   claimed;        // matched a sighting this frame
    };

    bool                  g_tracking  = false;
    bool                  g_cValid    = false;   // g_c holds the client's constants
    bool                  g_merged    = false;   // this frame's sightings are in
    unsigned              g_trackReads = 0;
    unsigned              g_cacheAge  = 0;
    std::vector<Sighting> g_sightings;
    std::vector<Tracked>  g_tracked;

    float Dist3(const float a[3], const float b[3])
    {
        const float d[3] = { a[0] - b[0], a[1] - b[1], a[2] - b[2] };
        return Len3(d);
    }

    void AddSighting(const float camRel[3], const float colour[3], float reach, int kind)
    {
        if (!g_haveCam || g_sightings.size() >= kMaxSightings)
            return;
        float abs[3];
        for (int i = 0; i < 3; ++i)
            abs[i] = camRel[i] + g_cam[i];
        // The client sets the same lights again for every model it draws near them: about 70 calls a
        // frame for five lights in Darkshire.
        for (Sighting& s : g_sightings)
            if (s.kind == kind && Dist3(s.abs, abs) <= kSame)
                return;
        Sighting s;
        memcpy(s.abs, abs, sizeof(abs));
        memcpy(s.colour, colour, sizeof(s.colour));
        s.reach = reach;
        s.kind  = kind;
        g_sightings.push_back(s);
    }

    void TrackLight(const D3DLIGHT9& L)
    {
        if (L.Type != D3DLIGHT_POINT)
            return;
        EnsureFrame();
        const float camRel[3] = { L.Position.x, L.Position.y, L.Position.z };
        if (Len3(camRel) > g_cfg.lamps.maxDistance + 40.0f)
            return;
        const float colour[3] = { L.Diffuse.r, L.Diffuse.g, L.Diffuse.b };
        if (std::max(colour[0], std::max(colour[1], colour[2])) < 0.02f)
            return;
        const float att[3] = { L.Attenuation0, L.Attenuation1, L.Attenuation2 };
        float reach = Reach(att);
        if (!(reach > 0.0f))
            reach = L.Range > 0.0f ? L.Range : 15.0f;
        reach = std::min(std::max(reach, 3.0f), 40.0f);
        AddSighting(camRel, colour, reach, 0);
    }

    // A lamppost's glow sprite. The cheap tests come first: most additive draws are particles.
    void TrackSprite(IDirect3DDevice9* dev, const LampDraw& d)
    {
        if (!g_cfg.lamps.sprites || !d.vs || d.up || d.nv != 4)
            return;
        DWORD blend = 0, dst = 0;
        dev->lpVtbl->GetRenderState(dev, D3DRS_ALPHABLENDENABLE, &blend);
        if (!blend)
            return;
        dev->lpVtbl->GetRenderState(dev, D3DRS_DESTBLEND, &dst);
        if (dst != D3DBLEND_ONE)
            return;
        if (!g_cValid)
        {
            if (FAILED(dev->lpVtbl->GetVertexShaderConstantF(dev, 0, g_c, 256)))
                return;
            g_cValid = true;
        }
        // The colour an unlit M2 shader draws: (0.95 0.60 0.22) at a Darkshire lamppost. Warm only, which
        // leaves out most spell effects.
        float tint[3];
        for (int i = 0; i < 3; ++i)
            tint[i] = g_c[28 * 4 + i] + g_c[29 * 4 + i];
        const float top = std::max(tint[0], std::max(tint[1], tint[2]));
        if (top < 0.2f || tint[0] < tint[2] * 1.3f || tint[0] < tint[1])
            return;
        if (g_trackReads >= kTrackReads)
            return;
        ++g_trackReads;
        EnsureFrame();
        const ShaderInfo& sh = Analyse(d.vs);
        float centre[3], size = 0.0f;
        if (!GlowCentre(dev, d, &sh, centre, size))
            return;
        if (size < 0.2f || size > 4.0f || Len3(centre) > g_cfg.lamps.maxDistance)
            return;
        const float colour[3] = { tint[0] * g_cfg.lamps.spriteGain, tint[1] * g_cfg.lamps.spriteGain,
                                  tint[2] * g_cfg.lamps.spriteGain };
        AddSighting(centre, colour, g_cfg.lamps.spriteReach, 1);
    }

    void Merge()
    {
        if (g_merged)
            return;
        g_merged = true;
        const double now = Now();
        static double last = 0.0;
        const double dt = last > 0.0 && now - last < 1.0 ? now - last : 0.0;
        last = now;
        EnsureFrame();
        for (Tracked& t : g_tracked)
            t.claimed = false;
        for (const Sighting& s : g_sightings)
        {
            if (s.kind == 1)
            {
                bool nearLight = false;
                for (const Sighting& o : g_sightings)
                    if (o.kind == 0 && Dist3(o.abs, s.abs) <= kNearLight) { nearLight = true; break; }
                if (nearLight)
                    continue;
            }
            Tracked* best = nullptr;
            float bestD = kFollow[s.kind];
            for (Tracked& t : g_tracked)
            {
                if (t.kind != s.kind || t.claimed)
                    continue;
                const float dd = Dist3(t.abs, s.abs);
                if (dd <= bestD) { bestD = dd; best = &t; }
            }
            if (!best)
            {
                if (g_tracked.size() >= kMaxTracked)
                {
                    // Full: the one unseen longest makes room.
                    auto oldest = std::min_element(g_tracked.begin(), g_tracked.end(),
                        [](const Tracked& a, const Tracked& b) { return a.lastSeen < b.lastSeen; });
                    g_tracked.erase(oldest);
                }
                Tracked t = {};
                memcpy(t.origin, s.abs, sizeof(t.origin));
                t.kind = s.kind;
                t.born = now;
                g_tracked.push_back(t);
                best = &g_tracked.back();
            }
            memcpy(best->abs, s.abs, sizeof(best->abs));
            memcpy(best->colour, s.colour, sizeof(best->colour));
            best->reach    = s.reach;
            best->lastSeen = now;
            best->unseen   = 0.0;
            best->claimed  = true;
            if (best->kind == 1 && Dist3(best->abs, best->origin) > kStill)
                best->mobile = true;
        }
        g_sightings.clear();
        // A sprite that moved is kept too, only never drawn: dropped, it would be found again as a new one.
        // Unseen time counts only on screen, where the client would have named it.
        const float forget = g_cfg.lamps.maxDistance + kForget;
        for (Tracked& t : g_tracked)
        {
            if (t.claimed || !g_haveCam)
                continue;
            float camRel[3], uv[2];
            for (int i = 0; i < 3; ++i)
                camRel[i] = t.abs[i] - g_cam[i];
            ScreenUV(camRel, uv);
            if (uv[0] >= kEdge && uv[0] <= 1.0f - kEdge && uv[1] >= kEdge && uv[1] <= 1.0f - kEdge)
                t.unseen += dt;
            if (Len3(camRel) > forget)
                t.unseen = 1e9;
        }
        const double keep = g_cfg.lamps.keep;
        g_tracked.erase(std::remove_if(g_tracked.begin(), g_tracked.end(),
            [&](const Tracked& t) { return t.unseen > keep; }), g_tracked.end());
    }

    // ---------------------------------------------------------------------------------------------
    // the report

    void SortByDistance(PlaceList& list)
    {
        std::sort(list.places.begin(), list.places.end(),
                  [](const Place& a, const Place& b) { return a.dist < b.dist; });
    }

    void Report()
    {
        float player[3] = {};
        const bool havePlayer = ClientPlayer(player);
        float hour = 0.0f;
        const bool haveHour = ClientHour(hour);
        Log("lamps: --- report of probe %llu, %u frames ---", g_probeNumber, kWindow);
        Log("lamps: camera (%.1f %.1f %.1f)%s, player (%.1f %.1f %.1f)%s, game time %02d:%02d%s",
            g_cam[0], g_cam[1], g_cam[2], g_haveCam ? "" : " (not read)", player[0], player[1], player[2],
            havePlayer ? "" : " (not read)", static_cast<int>(hour), static_cast<int>(hour * 60.0f) % 60,
            haveHour ? "" : " (no clock)");
        Log("lamps: %u world draws in the window", g_nDraws);

        SortByDistance(g_shaderPlaces);
        Log("lamps: SHADER LIGHTS: %u of %u draws through a shader that can take a light had one lit; %u places "
            "(%u lights not placed, %u sightings with no room)",
            g_nLit, g_nLightable, static_cast<unsigned>(g_shaderPlaces.places.size()), g_nShaderUnplaced,
            g_shaderPlaces.overflow);
        unsigned shown = 0;
        for (const Place& p : g_shaderPlaces.places)
        {
            if (shown++ >= kReportLines) break;
            Log("lamps:   %6.1f yd  screen (%.3f %.3f)  at (%.1f %.1f %.1f)  colour (%.2f %.2f %.2f)  "
                "attenuation (%.4f %.4f %.5f)  reach %.1f yd  slot %s  frames %u/%u  draws %u",
                p.dist, p.uv[0], p.uv[1], p.abs[0], p.abs[1], p.abs[2], p.colour[0], p.colour[1], p.colour[2],
                p.att[0], p.att[1], p.att[2], Reach(p.att),
                p.slots == 3 ? "1+2" : p.slots == 2 ? "2" : "1", p.frames, kWindow, p.draws);
        }

        SortByDistance(g_fixedPlaces);
        Log("lamps: FIXED LIGHTS: %u SetLight calls; %u fixed-function draws with LIGHTING on, %u of them with a "
            "point or spot light enabled; %u places (%u sightings with no room)",
            g_nSetLight, g_nFfLit, g_nFfLocal, static_cast<unsigned>(g_fixedPlaces.places.size()),
            g_fixedPlaces.overflow);
        char en[256] = "";
        int len = 0;
        for (int i = 0; i < 9; ++i)
            if (g_nEnableOn[i] || g_nEnableOff[i])
                len += _snprintf_s(en + len, sizeof(en) - len, _TRUNCATE, " [%d%s] on %u off %u", i,
                                   i == 8 ? "+" : "", g_nEnableOn[i], g_nEnableOff[i]);
        Log("lamps:   LightEnable calls:%s", len ? en : " none");
        shown = 0;
        for (const Place& p : g_fixedPlaces.places)
        {
            if (shown++ >= kReportLines) break;
            Log("lamps:   %6.1f yd  screen (%.3f %.3f)  %s  raw (%.1f %.1f %.1f)  as camera-relative at "
                "(%.1f %.1f %.1f)  diffuse (%.2f %.2f %.2f)  range %.1f  attenuation (%.4f %.4f %.5f)  "
                "frames %u/%u  draws lit %u",
                p.dist, p.uv[0], p.uv[1], p.kind == D3DLIGHT_SPOT ? "spot " : "point", p.raw[0], p.raw[1],
                p.raw[2], p.abs[0], p.abs[1], p.abs[2], p.colour[0], p.colour[1], p.colour[2], p.range,
                p.att[0], p.att[1], p.att[2], p.frames, kWindow, p.draws);
        }

        SortByDistance(g_glowPlaces);
        Log("lamps: GLOWS: %u additive draws (%u through a shader); %u places; %u not read (%u a frame at most, "
            "or no readable position); %u sightings with no room",
            g_nAdditive, g_nShaderAdditive, static_cast<unsigned>(g_glowPlaces.places.size()), g_nUnread,
            kReadsPerFrame, g_glowPlaces.overflow);
        shown = 0;
        for (const Place& p : g_glowPlaces.places)
        {
            if (shown++ >= kReportLines) break;
            const auto it = g_texs.find(p.tex);
            const TexInfo ti = it != g_texs.end() ? it->second : TexInfo();
            Log("lamps:   %6.1f yd  screen (%.3f %.3f)  at (%.1f %.1f %.1f)  size %.2f yd  %s  %u verts  "
                "tex %p %ux%u %s colour (%.2f %.2f %.2f)  tint (%.2f %.2f %.2f %.2f)  src blend %u  "
                "black fog %u/%u  frames %u/%u",
                p.dist, p.uv[0], p.uv[1], p.abs[0], p.abs[1], p.abs[2], p.size, p.shader ? "M2" : "ff",
                p.verts, p.tex, ti.w, ti.h, FormatName(ti.fmt), p.colour[0], p.colour[1], p.colour[2],
                p.tint[0], p.tint[1], p.tint[2], p.tint[3], p.kind, p.blackFog, p.draws, p.frames, kWindow);
        }
        Log("lamps: --- end of report ---");
    }
}

// -------------------------------------------------------------------------------------------------

void LampsProbe(IDirect3DDevice9* dev)
{
    if (g_active)
    {
        Log("lamps: a window is already open (frame %u of %u)", g_windowFrame, kWindow);
        return;
    }
    ClearWindow();
    // The constants the client set before the window opened; every upload after this keeps them current.
    if (FAILED(dev->lpVtbl->GetVertexShaderConstantF(dev, 0, g_c, 256)))
        memset(g_c, 0, sizeof(g_c));
    g_active = true;
    ++g_probeNumber;
    Log("lamps: probe %llu opened for %u frames; stand still. Each source is logged as it is first seen, "
        "then summed up in one report", g_probeNumber, kWindow);
}

bool LampsActive()
{
    return g_active;
}

void LampsSetTracking(bool on)
{
    if (on == g_tracking)
        return;
    g_tracking = on;
    if (!on)
    {
        // The mirror stops following the client, so it is read again before it is next used.
        g_cValid = false;
        g_sightings.clear();
        g_tracked.clear();
    }
}

bool LampsTracking()
{
    return g_tracking;
}

bool LampsWanted()
{
    return g_active || g_tracking;
}

void LampsConstants(UINT reg, const float* data, UINT count)
{
    if (!LampsWanted() || !data || reg >= 256)
        return;
    if (reg + count > 256)
        count = 256 - reg;
    memcpy(&g_c[reg * 4], data, count * 4 * sizeof(float));
}

void LampsSetLight(DWORD index, const D3DLIGHT9* L, bool inWorld)
{
    if (!L)
        return;
    if (g_tracking && inWorld)
        TrackLight(*L);
    if (!g_active)
        return;
    EnsureFrame();
    ++g_nSetLight;
    if (g_windowFrame == 0 && g_rawFixed < kRawLines)   // the client repeats the same calls every frame
    {
        ++g_rawFixed;
        Log("lamps: raw SetLight %u, frame %u: type %d, position (%.2f %.2f %.2f), direction (%.3f %.3f %.3f), "
            "diffuse (%.2f %.2f %.2f), ambient (%.2f %.2f %.2f), range %.1f, attenuation (%.4f %.4f %.5f)",
            index, g_windowFrame, static_cast<int>(L->Type), L->Position.x, L->Position.y, L->Position.z,
            L->Direction.x, L->Direction.y, L->Direction.z, L->Diffuse.r, L->Diffuse.g, L->Diffuse.b,
            L->Ambient.r, L->Ambient.g, L->Ambient.b, L->Range, L->Attenuation0, L->Attenuation1,
            L->Attenuation2);
    }
    FixedLight(*L, false);
}

void LampsLightEnable(DWORD index, BOOL on)
{
    if (!g_active)
        return;
    const DWORD i = index < 8 ? index : 8;
    if (on) ++g_nEnableOn[i];
    else    ++g_nEnableOff[i];
}

void LampsDraw(IDirect3DDevice9* dev, const LampDraw& d)
{
    if (g_tracking)
        TrackSprite(dev, d);
    if (!g_active)
        return;
    EnsureFrame();
    ++g_nDraws;
    const ShaderInfo* sh = d.vs ? &Analyse(d.vs) : nullptr;
    if (sh && (sh->light[0] || sh->light[1]))
        ShaderLights(d.vs, *sh);
    if (!sh)
        FixedDraw(dev);
    Glow(dev, d, sh);
}

void LampsWorldEnded()
{
    if (g_tracking)
        Merge();
}

int LampsGather(const float cam[3], LampLight* out, int max)
{
    if (!g_tracking || max <= 0)
        return 0;
    Merge();
    const double now   = Now();
    const double keep  = g_cfg.lamps.keep;
    const float  reach = g_cfg.lamps.maxDistance;
    int n = 0;
    for (const Tracked& t : g_tracked)
    {
        if (t.mobile)
            continue;
        LampLight l;
        for (int i = 0; i < 3; ++i)
            l.pos[i] = t.abs[i] - cam[i];
        l.dist = Len3(l.pos);
        if (l.dist > reach + t.reach)
            continue;
        const double age    = now - t.born - (t.kind == 1 ? kSpriteDelay : 0.0);
        const double unseen = t.unseen;
        double fade = std::min(1.0, std::max(0.0, age / kFadeIn[t.kind]));
        if (unseen > kGrace)
            fade *= std::max(0.0, 1.0 - (unseen - kGrace) / std::max(keep - kGrace, 0.01));
        if (fade <= 0.0)
            continue;
        for (int i = 0; i < 3; ++i)
            l.colour[i] = t.colour[i] * static_cast<float>(fade);
        l.reach = t.reach;
        l.kind  = t.kind;
        // Nearest first; the list is short, so an insertion keeps it sorted.
        int at = n < max ? n : max;
        while (at > 0 && out[at - 1].dist > l.dist)
        {
            if (at < max)
                out[at] = out[at - 1];
            --at;
        }
        if (at < max)
        {
            out[at] = l;
            if (n < max)
                ++n;
        }
    }
    // The lights from the buildings' files ([lamps] files): as lampposts are (kind 1, lighting the surfaces
    // near them too), and not where a light the client shows is already within 2 yards: a torch the client
    // lights would be counted twice.
    if (g_cfg.lamps.files && g_cfg.shadow.mapTerrain)
    {
        static MapLight file[256];
        const int nf = MapLightsNear(cam, reach + 20.0f, file, 256);
        for (int f = 0; f < nf; ++f)
        {
            bool dup = false;
            for (const Tracked& t : g_tracked)
            {
                const float dx = t.abs[0] - file[f].pos[0], dy = t.abs[1] - file[f].pos[1], dz = t.abs[2] - file[f].pos[2];
                if (dx * dx + dy * dy + dz * dz < 4.0f) { dup = true; break; }
            }
            if (dup)
                continue;
            LampLight l;
            for (int i = 0; i < 3; ++i)
                l.pos[i] = file[f].pos[i] - cam[i];
            l.dist = Len3(l.pos);
            if (l.dist > reach + file[f].reach)
                continue;
            for (int i = 0; i < 3; ++i)
                l.colour[i] = file[f].colour[i] * g_cfg.lamps.spriteGain;
            l.reach = file[f].reach;
            l.kind  = 1;
            int at = n < max ? n : max;
            while (at > 0 && out[at - 1].dist > l.dist)
            {
                if (at < max)
                    out[at] = out[at - 1];
                --at;
            }
            if (at < max)
            {
                out[at] = l;
                if (n < max)
                    ++n;
            }
        }
    }
    return n;
}

unsigned LampsTracked()
{
    return static_cast<unsigned>(g_tracked.size());
}

void LampsFrameEnd()
{
    g_frameReady = false;
    if (g_tracking)
    {
        Merge();              // a frame with no world end: its sightings still count
        g_merged = false;
        g_trackReads = 0;
        if (++g_cacheAge >= kCacheFrames && !g_active)
        {
            g_cacheAge = 0;
            g_shaders.clear();
            g_decls.clear();
        }
    }
    if (!g_active)
        return;
    if (++g_windowFrame < kWindow)
        return;
    Report();
    g_active = false;
    ClearWindow();
}

void LampsReset()
{
    if (g_active)
        Log("lamps: the device went away, so the open window is dropped");
    g_active = false;
    ClearWindow();
    g_cValid = false;
    g_merged = false;
    g_sightings.clear();
    g_tracked.clear();
}
