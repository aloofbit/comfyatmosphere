// client: reading the running client: where the camera and the local player are.
//
// Both addresses were found by disassembling this WoW.exe and verified by comfygrass (see its README):
// the camera's world position at 0x00C7CF20, and the object manager at 0x00B41414 walked to the local
// player, whose position sits at +0x9B8. All reads are guarded: a loading screen or another build fails
// a read and the caller carries on without.

#define WIN32_LEAN_AND_MEAN

#include <windows.h>

#include "client.h"
#include "config.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace
{
    bool SafeCopy(uintptr_t src, void* dst, size_t n)
    {
        __try
        {
            memcpy(dst, reinterpret_cast<const void*>(src), n);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    intptr_t Slide()
    {
        static const intptr_t slide = reinterpret_cast<intptr_t>(GetModuleHandleW(nullptr)) - 0x00400000;
        return slide;
    }

    bool SaneWorld(const float p[3])
    {
        for (int i = 0; i < 3; ++i)
            if (!(p[i] == p[i]) || p[i] < -20000.0f || p[i] > 20000.0f)
                return false;
        return true;
    }

}

bool ClientCamera(float cam[3])
    {
        const ClientSettings& b = g_cfg.client;
        if (!b.camAddr || !SafeCopy(static_cast<uintptr_t>(b.camAddr + Slide()), cam, 12))
            return false;
        return SaneWorld(cam) && !(cam[0] == 0.0f && cam[1] == 0.0f && cam[2] == 0.0f);
    }

static bool ClientPlayerRaw(float pos[3]);

// The local player out of the object manager: the same walk as comfygrass's FindLocalPlayerObject.
//
// On a ship or a zeppelin (2026-10-02) the client keeps the position relative to the ship: at Theramore, the
// camera at (-4002.7 -4729.0 10.2) and the player at (-1.4 -10.3 6.1). The fog took its ground from the
// terrain near the map's centre, far over the deck, and filled the ship. The camera is in world coordinates
// and never more than 50 yards from the player, so a player more than kOffShip yards across from it is on a
// ship, and the camera's position is given instead (*onShip set). The ship's own place is not read.
bool ClientPlayer(float pos[3], bool* onShip)
    {
        constexpr float kOffShip = 200.0f;
        if (onShip)
            *onShip = false;
        float cam[3];
        if (!ClientPlayerRaw(pos))
            return false;
        if (ClientCamera(cam) && hypotf(pos[0] - cam[0], pos[1] - cam[1]) > kOffShip)
        {
            memcpy(pos, cam, sizeof(cam));
            if (onShip)
                *onShip = true;
        }
        return true;
    }

// The local player's object in the object manager, or 0.
static DWORD PlayerObject()
    {
        const ClientSettings& b = g_cfg.client;
        if (!b.objMgrAddr)
            return 0;
        DWORD mgr = 0;
        if (!SafeCopy(static_cast<uintptr_t>(b.objMgrAddr + Slide()), &mgr, 4) || !mgr)
            return 0;
        DWORD guid[2] = {}, link = 0, obj = 0;
        if (!SafeCopy(mgr + 0xC0, guid, 8) || (!guid[0] && !guid[1]))
            return 0;
        if (!SafeCopy(mgr + 0xA4, &link, 4) || !SafeCopy(mgr + 0xAC, &obj, 4))
            return 0;
        for (int n = 0; n < 16384 && obj && !(obj & 1); ++n)
        {
            DWORD g[2] = {};
            if (!SafeCopy(obj + 0x30, g, 8))
                return 0;
            if (g[0] == guid[0] && g[1] == guid[1])
                return obj;
            DWORD next = 0;
            if (!SafeCopy(obj + link + 4, &next, 4))
                return 0;
            obj = next;
        }
        return 0;
    }

// The position as the player object holds it: in the world, or relative to a ship.
static bool ClientPlayerRaw(float pos[3])
    {
        const DWORD obj = g_cfg.client.playerPosOff ? PlayerObject() : 0;
        return obj && SafeCopy(obj + g_cfg.client.playerPosOff, pos, 12) && SaneWorld(pos);
    }

// Whether the local player rides a mount (2026-10-02): UNIT_FIELD_MOUNTDISPLAYID, update field 0x85 (OBJECT_END
// 6, the auras to 0x7C, then the attack times, the bounding radius, the combat reach and the two display ids),
// read through the fields pointer at +0x8 as ClientGameObjects reads a game object's.
bool ClientPlayerMounted(bool& mounted)
    {
        const DWORD obj = PlayerObject();
        DWORD fields = 0, display = 0;
        if (!obj || !SafeCopy(obj + 0x8, &fields, 4) || !fields || !SafeCopy(fields + 0x85 * 4, &display, 4))
            return false;
        mounted = display != 0;
        return true;
    }

// Every unit (creature, NPC) and player out of the object manager: the same walk, keeping each object of
// type 3 (unit) or 4 (player), its type at +0x14; units keep their position where the player does.
int ClientUnits(float (*out)[3], int max)
    {
        const ClientSettings& b = g_cfg.client;
        if (!b.objMgrAddr || !b.playerPosOff || max <= 0)
            return 0;
        DWORD mgr = 0;
        if (!SafeCopy(static_cast<uintptr_t>(b.objMgrAddr + Slide()), &mgr, 4) || !mgr)
            return 0;
        DWORD link = 0, obj = 0;
        if (!SafeCopy(mgr + 0xA4, &link, 4) || !SafeCopy(mgr + 0xAC, &obj, 4))
            return 0;
        int n = 0;
        for (int i = 0; i < 16384 && obj && !(obj & 1) && n < max; ++i)
        {
            DWORD type = 0;
            if (!SafeCopy(obj + 0x14, &type, 4))
                break;
            if ((type == 3 || type == 4) && SafeCopy(obj + b.playerPosOff, out[n], 12) && SaneWorld(out[n]))
                ++n;
            DWORD next = 0;
            if (!SafeCopy(obj + link + 4, &next, 4))
                break;
            obj = next;
        }
        return n;
    }

// The same walk with each unit's display ids (2026-10-04), from its update fields (the pointer at +0x8), as
// ClientPlayerMounted reads the mount: the shadow cache tells a unit's own model by the model it wears.
int ClientUnitList(ClientUnit* out, int max)
    {
        const ClientSettings& b = g_cfg.client;
        if (!b.objMgrAddr || !b.playerPosOff || max <= 0)
            return 0;
        DWORD mgr = 0;
        if (!SafeCopy(static_cast<uintptr_t>(b.objMgrAddr + Slide()), &mgr, 4) || !mgr)
            return 0;
        DWORD link = 0, obj = 0, me[2] = {};
        if (!SafeCopy(mgr + 0xA4, &link, 4) || !SafeCopy(mgr + 0xAC, &obj, 4) || !SafeCopy(mgr + 0xC0, me, 8))
            return 0;
        int n = 0;
        for (int i = 0; i < 16384 && obj && !(obj & 1) && n < max; ++i)
        {
            DWORD type = 0;
            if (!SafeCopy(obj + 0x14, &type, 4))
                break;
            ClientUnit& u = out[n];
            if ((type == 3 || type == 4) && SafeCopy(obj + b.playerPosOff, u.pos, 12) && SaneWorld(u.pos))
            {
                // The fields from UNIT_FIELD_DISPLAYID (0x83) to UNIT_FIELD_BYTES_1 (0x8A).
                DWORD fields = 0, f[8] = {}, guid[2] = {};
                if (!SafeCopy(obj + 0x8, &fields, 4) || !fields || !SafeCopy(fields + 0x83 * 4, f, sizeof(f)))
                    memset(f, 0, sizeof(f));
                u.display   = f[0];
                u.mount     = f[2];
                u.bytes1    = f[7];
                u.stealthed = ((f[7] >> 24) & 0x02) != 0;
                u.self      = SafeCopy(obj + 0x30, guid, 8) && guid[0] == me[0] && guid[1] == me[1];
                ++n;
            }
            DWORD next = 0;
            if (!SafeCopy(obj + link + 4, &next, 4))
                break;
            obj = next;
        }
        return n;
    }

// Every game object (type 5) out of the object manager, by the same walk (2026-10-01). Its fields (the pointer at
// +0x8) are 1.12's update fields: the scale at 0x4, then GAMEOBJECT_DISPLAYID at 0x8 and POS_X, POS_Y, POS_Z,
// FACING at 0xF..0x12. In Stormwind's Trade District a torch and two lampposts are game objects: no map file
// places them, so the lamps had no light for them.
int ClientGameObjects(ClientObject* out, int max)
    {
        const ClientSettings& b = g_cfg.client;
        if (!b.objMgrAddr || max <= 0)
            return 0;
        DWORD mgr = 0;
        if (!SafeCopy(static_cast<uintptr_t>(b.objMgrAddr + Slide()), &mgr, 4) || !mgr)
            return 0;
        DWORD link = 0, obj = 0;
        if (!SafeCopy(mgr + 0xA4, &link, 4) || !SafeCopy(mgr + 0xAC, &obj, 4))
            return 0;
        int n = 0;
        for (int i = 0; i < 16384 && obj && !(obj & 1) && n < max; ++i)
        {
            DWORD type = 0;
            if (!SafeCopy(obj + 0x14, &type, 4))
                break;
            DWORD fields = 0;
            uint32_t f[0x13];
            if (type == 5 && SafeCopy(obj + 0x8, &fields, 4) && fields && SafeCopy(fields, f, sizeof(f)))
            {
                ClientObject& o = out[n];
                memcpy(&o.scale, &f[0x4], 4);
                o.display = f[0x8];
                memcpy(o.pos, &f[0xF], 12);
                memcpy(&o.facing, &f[0x12], 4);
                if (SaneWorld(o.pos) && o.scale > 0.01f && o.scale < 20.0f && o.facing == o.facing && o.display &&
                    o.display < 1000000)
                    ++n;
            }
            DWORD next = 0;
            if (!SafeCopy(obj + link + 4, &next, 4))
                break;
            obj = next;
        }
        return n;
    }

// The current map's folder name under World\Maps: the buffer the client formats its tile names with
// ("%s\%s_%d_%d.adt" at 0x0086C368, called with it as the second %s). Letters, digits and underscores.
bool ClientMapName(char* out, int size)
    {
        const ClientSettings& b = g_cfg.client;
        char buf[64] = {};
        if (!b.mapNameAddr || size < 2 || !SafeCopy(static_cast<uintptr_t>(b.mapNameAddr + Slide()), buf, sizeof(buf) - 1))
            return false;
        int n = 0;
        for (; buf[n]; ++n)
        {
            const char c = buf[n];
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') || n >= size - 1)
                return false;
            out[n] = c;
        }
        out[n] = 0;
        return n > 0;
    }

// The game clock: the time of day as a fraction of the day, a float that carries the seconds. comfytime
// found it by measurement against the minimap clock (its timeofday.cpp) and writes it to set the time,
// so this reads the time comfytime shows as well.
bool ClientHour(float& hour)
    {
        const ClientSettings& b = g_cfg.client;
        float f = 0.0f;
        if (!b.clockAddr || !SafeCopy(static_cast<uintptr_t>(b.clockAddr + Slide()), &f, 4))
            return false;
        if (!(f >= 0.0f && f <= 1.0f))
            return false;
        hour = f * 24.0f;
        return true;
    }
