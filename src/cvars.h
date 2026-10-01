// The in-game controls: CVars the ComfyAtmosphere addon sets from Video > Atmosphere. See cvars.cpp.
#pragma once

void CVarsAfterLoad();   // after LoadSettings: keep the ini values, then put the controls back on top
bool CVarsPoll();        // once a frame, at Present; true when a control changed a setting
// A line for the game's chat (2026-09-30). The DLL cannot write chat, so it registers comfyNotice<n>, n from
// 1 up, holding the line, and the addon prints each new one. Queued until the CVars are registered.
void CVarsNotice(const char* text);
