// Reading the running client: the camera's and the local player's world positions. See client.cpp.
#pragma once

bool ClientCamera(float cam[3]);      // camera world position
bool ClientPlayer(float pos[3]);      // local player world position (feet)
bool ClientHour(float& hour);         // the game's time of day, 0..24
