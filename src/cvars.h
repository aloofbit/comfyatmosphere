// The in-game controls: CVars the ComfyAtmosphere addon sets from Video > Shaders. See cvars.cpp.
#pragma once

void CVarsAfterLoad();   // after LoadSettings: keep the ini values, then put the controls back on top
bool CVarsPoll();        // once a frame, at Present; true when a control changed a setting
