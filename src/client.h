// Reading the running client: the camera's and the local player's world positions. See client.cpp.
#pragma once

bool ClientCamera(float cam[3]);      // camera world position
bool ClientPlayer(float pos[3], bool* onShip = nullptr);   // local player world position (feet); on a ship
                                                          // or zeppelin, the camera's (see client.cpp)
bool ClientPlayerMounted(bool& mounted);   // whether the local player rides a mount
bool ClientHour(float& hour);         // the game's time of day, 0..24
void ClientFrameEnd();                // at Present: the player's object is looked up again next frame
// A unit or player with its model (2026-10-04): UNIT_FIELD_DISPLAYID (0x83) and UNIT_FIELD_MOUNTDISPLAYID (0x85),
// both CreatureDisplayInfo.dbc rows; mount 0 on foot. stealthed: UNIT_FIELD_BYTES_1 (0x8A) byte 3 has
// UNIT_BYTE1_FLAGS_CREEP (0x02), which the server sets with a stealth aura. self: the local player.
struct ClientUnit { float pos[3]; unsigned display, mount, bytes1; bool stealthed, self; };
int  ClientUnitList(ClientUnit* out, int max);   // every unit and player with its display ids; the count
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
// The probe (2026-10-06): every transport game object (type 15, a ship or a zeppelin; 11, an elevator), its update
// fields, and each place in its own memory within 400 yards of at, to find where a ship under way is kept.
void ClientTransportsLog(const float at[3]);
