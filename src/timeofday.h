// The time of day (timeofday.cpp, from comfytime, 2026-10-07): finding the client's clock in memory, and
// writing a chosen time into it.
#pragma once

void TimeAttach();      // at attach: an old comfytime.dll still loaded keeps ours off

void TimeScanStart();   // snapshot now; narrowing follows by itself over the next few minutes
void TimeScanTick();    // once a frame

void TimeApply(const char* where);   // write the chosen time, if [time] enabled (Present and BeginScene)
void TimeStep(float hours);          // move the chosen time (Ctrl+PageUp/PageDown)
void TimeToggleDayNight();           // go to dayHour or nightHour, whichever is further away (Ctrl+End)
void TimeReload();                   // after the ini is reloaded (F11)
void TimeAfterTune();                // after /atmos: TimeReload, but only when a [time] value changed
float TimeCurrentHour();             // the time being shown, for writing back to the ini
void TimeSet(float hour);            // show this hour (the debug panel's slider)
void TimeSaveHour();                 // write the time being shown into [time] hour (Ctrl+Home, Save hour)
// What the time does now: 0 the game's own time ([time] enabled 0), 1 ours, 2 an old comfytime.dll sets it,
// 3 the addresses did not check out.
int TimeState();
void TimeProbe();                    // the probe (F12): the hour shown, and whether the addresses passed
