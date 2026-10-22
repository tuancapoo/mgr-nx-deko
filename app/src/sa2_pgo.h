// Sonic Adventure 2: profile collection of the PGO-instrumented Switch build (SA2_PGO=generate), see sa2_pgo.cpp.
// In every other build these do nothing.

#pragma once

namespace sa2::pgo {

// Picks this session's file (pgo/sa2_pgo_<N>.stream next to the executable) and starts the 2-minute dumps.
void Start();
// Writes every count so far to this session's file (the whole session each time, replacing the previous dump).
void DumpNow();
// The last dump, then the dump thread ends.
void Stop();

}  // namespace sa2::pgo
