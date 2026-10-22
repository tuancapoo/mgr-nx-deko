// Sonic Adventure 2: CPU cycles per thread over a stretch of the run (PC diagnostic, see sa2_thread_cycles_pc.h).
#include "sa2_thread_cycles_pc.h"

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>

namespace sa2::profile {

std::vector<ThreadCycles> SnapshotThreadCycles() {
  std::vector<ThreadCycles> out;
  const DWORD process = GetCurrentProcessId();
  const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return out;
  THREADENTRY32 entry{};
  entry.dwSize = sizeof(entry);
  for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
    if (entry.th32OwnerProcessID != process) continue;
    const HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
    if (!thread) continue;
    ThreadCycles t;
    t.id = entry.th32ThreadID;
    ULONG64 cycles = 0;
    if (QueryThreadCycleTime(thread, &cycles)) t.cycles = cycles;
    PWSTR description = nullptr;
    if (SUCCEEDED(GetThreadDescription(thread, &description)) && description) {
      for (const wchar_t* p = description; *p; ++p) t.name += *p < 128 ? char(*p) : '?';
      LocalFree(description);
    }
    CloseHandle(thread);
    out.push_back(std::move(t));
  }
  CloseHandle(snapshot);
  return out;
}

}  // namespace sa2::profile

#else

namespace sa2::profile {
std::vector<ThreadCycles> SnapshotThreadCycles() {
  return {};
}
}  // namespace sa2::profile

#endif
