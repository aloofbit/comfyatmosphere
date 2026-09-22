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
        const BeamsSettings& b = g_cfg.beams;
        if (!b.camAddr || !SafeCopy(static_cast<uintptr_t>(b.camAddr + Slide()), cam, 12))
            return false;
        return SaneWorld(cam) && !(cam[0] == 0.0f && cam[1] == 0.0f && cam[2] == 0.0f);
    }

// The local player out of the object manager: the same walk as comfygrass's FindLocalPlayerObject.
bool ClientPlayer(float pos[3])
    {
        const BeamsSettings& b = g_cfg.beams;
        if (!b.objMgrAddr || !b.playerPosOff)
            return false;
        DWORD mgr = 0;
        if (!SafeCopy(static_cast<uintptr_t>(b.objMgrAddr + Slide()), &mgr, 4) || !mgr)
            return false;
        DWORD guid[2] = {}, link = 0, obj = 0;
        if (!SafeCopy(mgr + 0xC0, guid, 8) || (!guid[0] && !guid[1]))
            return false;
        if (!SafeCopy(mgr + 0xA4, &link, 4) || !SafeCopy(mgr + 0xAC, &obj, 4))
            return false;
        for (int n = 0; n < 16384 && obj && !(obj & 1); ++n)
        {
            DWORD g[2] = {};
            if (!SafeCopy(obj + 0x30, g, 8))
                return false;
            if (g[0] == guid[0] && g[1] == guid[1])
                return SafeCopy(obj + b.playerPosOff, pos, 12) && SaneWorld(pos);
            DWORD next = 0;
            if (!SafeCopy(obj + link + 4, &next, 4))
                return false;
            obj = next;
        }
        return false;
    }

