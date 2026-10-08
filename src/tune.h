// The /atmos command: read and set any comfyatmos.ini value from the game's chat. See tune.cpp.
#pragma once

#include <map>
#include <string>
#include <vector>

// Runs one command (the text after "/atmos") and returns the lines to show in chat. `reloaded` is set
// when the command changed the settings and LoadSettings has run again, so the controls must be laid
// over them once more (CVarsAfterLoad).
std::vector<std::string> TuneRun(const std::string& command, bool& reloaded);

// Writes "section.key" -> value into comfyatmos.ini as /atmos save does, keeping each line's comment. The
// settings are not read again here.
bool TuneWriteIni(const std::map<std::string, std::string>& values, std::string& error);

void ProbeArm();   // in comfyatmos.cpp: log the next frame, as F12 does
void BenchArm();   // in comfyatmos.cpp: run the benchmark from the next frame, as Alt+F12 does
