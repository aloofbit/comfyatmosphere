// report: what this client is made of, for comfyatmos.log. See report.cpp.
#pragma once

#include <d3d9.h>

// Once for each device the client makes: reads the graphics card and the back buffer. The first one also
// starts the report.
void ReportDevice(IDirect3DDevice9* dev);
// Writes the report into comfyatmos.log on a thread of its own. `why` names the occasion ("F12").
void ReportStart(const char* why);
