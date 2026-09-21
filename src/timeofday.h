// Finding the client's time of day in memory. See timeofday.cpp.
#pragma once

void TimeScanStart();   // snapshot now; narrowing follows by itself over the next few minutes
void TimeScanTick();    // once a frame

void TimeApply(const char* where);   // write the chosen time, if [time] enabled (Present and BeginScene)
void TimeStep(float hours);          // move the chosen time (Ctrl+PageUp/PageDown)
void TimeReload();                   // after the ini is reloaded
