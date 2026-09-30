// Reading the running client: the camera's and the local player's world positions. See client.cpp.
#pragma once

bool ClientCamera(float cam[3]);      // camera world position
bool ClientPlayer(float pos[3]);      // local player world position (feet)
bool ClientHour(float& hour);         // the game's time of day, 0..24
int  ClientUnits(float (*out)[3], int max);   // every unit and player's world position (feet); the count
bool ClientMapName(char* out, int size);   // the current map's folder name ("Azeroth", "Kalimdor")
