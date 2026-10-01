// Reading the running client: the camera's and the local player's world positions. See client.cpp.
#pragma once

bool ClientCamera(float cam[3]);      // camera world position
bool ClientPlayer(float pos[3]);      // local player world position (feet)
bool ClientHour(float& hour);         // the game's time of day, 0..24
int  ClientUnits(float (*out)[3], int max);   // every unit and player's world position (feet); the count
bool ClientMapName(char* out, int size);   // the current map's folder name ("Azeroth", "Kalimdor")

// A game object the server spawned (a torch, a lamppost, a brazier no map file places).
struct ClientObject
{
    float    pos[3];    // world position
    float    facing;    // radians about z, counter-clockwise from +x
    float    scale;
    unsigned display;   // GameObjectDisplayInfo.dbc row
};
int  ClientGameObjects(ClientObject* out, int max);   // every game object; the count
