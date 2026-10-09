// What this copy holds on the heap, by the code that allocated it (memtrace.cpp). Only in a build with
// -DCOMFY_MEMTRACE=ON; otherwise MemTraceReport does nothing.
#pragma once

#ifdef COMFY_MEMTRACE
void MemTraceReport(const char* when);
#else
inline void MemTraceReport(const char*) {}
#endif
