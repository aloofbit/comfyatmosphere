// mpq: reading files out of the client's MPQ archives. See mpq.cpp.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Opens every archive in the client's Data folder, once, on the first call. Not thread safe: one thread
// only (the terrain loader).
bool MpqOpen();
bool MpqOpenDir(const wchar_t* dataDir);   // the same from a folder named here (ends in a backslash)
// A file by its archive path (backslashes), from the archive that wins; false if no archive holds it or
// it is stored in a way this reader does not handle (encrypted, or not zlib).
bool MpqRead(const char* name, std::vector<uint8_t>& out);
// Every archive holding the name, best first, as "a, b, c" (for the probe).
std::string MpqHolders(const char* name);
unsigned MpqArchiveCount();
