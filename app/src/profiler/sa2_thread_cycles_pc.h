// Sonic Adventure 2: CPU cycles per thread over a stretch of the run (PC diagnostic).
//
// A lockstep capture (sa2_capture.cpp) executes the same game frames in every run, so the CPU cycles each thread
// spends between its first and last frame are the cost of the same work in two builds: the way to compare a
// build-time step on the PC when instruction counts cannot (a change inside loops makes a function longer and each
// iteration shorter). QueryThreadCycleTime counts the cycles a thread really ran, whatever else the PC is doing.
// It measures this PC's processor and compiler, not the Switch's: a direction, not the console's number.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sa2::profile {

struct ThreadCycles {
  unsigned long id = 0;
  std::string name;
  uint64_t cycles = 0;
};

// Every thread of this process with the cycles it has run so far (empty off Windows).
std::vector<ThreadCycles> SnapshotThreadCycles();

}  // namespace sa2::profile
