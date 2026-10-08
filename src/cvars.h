// The in-game controls: CVars the ComfyAtmosphere addon sets from Video > Atmosphere. See cvars.cpp.
#pragma once

#include <string>

void CVarsAfterLoad();   // after LoadSettings: keep the ini values, then put the controls back on top
bool CVarsPoll();        // once a frame, at Present; true when a control changed a setting
// A line for the game's chat (2026-09-30). The DLL cannot write chat, so it registers comfyNotice<n>, n from
// 1 up, holding the line, and the addon prints each new one. Queued until the CVars are registered.
void CVarsNotice(const char* text);
// The on-screen stats (/atmos stats): written into comfyStats in place, as name=value; pairs.
void CVarsStats(const std::string& text);
// The time of day from the debug panel and the key bindings (2026-10-07), once a frame: comfyTimeSet carries
// "<number> <verb> [value]" from the addon, and comfyTimeShown the hour back, written in place.
void CVarsTime();
// The controls' values as the client last gave them, as name=value, for the client report (report.cpp).
void CVarsControlsText(std::string& out);
