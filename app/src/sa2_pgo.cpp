// Sonic Adventure 2: profile collection for profile-guided optimisation (PGO).
//
// The instrumented Switch build (cmake -DSA2_PGO=generate) counts every branch of the game code and the app. GCC's
// usual way to save the counts (libgcov writing a .gcda file per object at exit, at the build machine's paths) does
// not fit the Switch: the process is often closed from the HOME menu without exiting, and those paths do not exist
// there. So the build registers each object's counts in a section (-fprofile-info-section=gcov_info) instead, and
// this file streams them all into one file, pgo/sa2_pgo_<N>.stream next to the NRO (N = this session):
//   - every 2 minutes, when the game loses focus (HOME menu) and at exit;
//   - the whole session each time (the counts only grow), replacing the previous dump through a .tmp file, so a dump
//     cut short (console switched off) leaves the previous one.
// On the PC, tools/win/pgo_merge.ps1 turns every session's stream into .gcda files next to the build's objects
// (gcov-tool merge-stream, which adds sessions together), and the next build with -DSA2_PGO=use reads them.

#include "sa2_pgo.h"

#if defined(SA2_PGO_GENERATE)

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

extern "C" {  // GCC's gcov.h has no C++ guard of its own
#include <gcov.h>
}

#include <rex/filesystem.h>

// One pointer per instrumented object, in the section GNU ld brackets with these two symbols.
extern "C" const struct gcov_info* const __start_gcov_info[];
extern "C" const struct gcov_info* const __stop_gcov_info[];

namespace sa2::pgo {
namespace {

constexpr auto kPeriod = std::chrono::minutes(2);

std::mutex g_dump_mutex;  // one dump at a time (the thread, focus loss, exit)
std::filesystem::path g_file;
std::mutex g_thread_mutex;
std::condition_variable g_wake;
bool g_stop = false;
std::thread g_thread;

struct Stream {
  std::FILE* file = nullptr;
  bool ok = true;
  std::vector<std::unique_ptr<unsigned char[]>> allocations;  // libgcov's scratch buffers for one dump
};

void Write(const void* data, unsigned length, void* arg) {
  auto* s = static_cast<Stream*>(arg);
  if (s->ok && length && std::fwrite(data, 1, length, s->file) != length) s->ok = false;
}

void Filename(const char* name, void* arg) {
  __gcov_filename_to_gcfn(name, Write, arg);
}

void* Allocate(unsigned length, void* arg) {
  auto* s = static_cast<Stream*>(arg);
  s->allocations.push_back(std::make_unique<unsigned char[]>(length ? length : 1));
  return s->allocations.back().get();
}

}  // namespace

void DumpNow() {
  std::lock_guard<std::mutex> lock(g_dump_mutex);
  if (g_file.empty()) return;
  std::filesystem::path temporary = g_file;
  temporary += ".tmp";
  Stream stream;
  stream.file = std::fopen(temporary.string().c_str(), "wb");
  if (!stream.file) return;
  static std::vector<char> buffer(1 << 20);  // one SD write per MB instead of per record
  std::setvbuf(stream.file, buffer.data(), _IOFBF, buffer.size());
  const gcov_info* const* info = __start_gcov_info;
  const gcov_info* const* const end = __stop_gcov_info;
  __asm__("" : "+r"(info));  // as in GCC's manual: the compiler must not reason about the linker's symbols
  for (; info != end && stream.ok; ++info) {
    if (*info) __gcov_info_to_gcda(*info, Filename, Write, Allocate, &stream);
  }
  const bool closed = std::fclose(stream.file) == 0;
  if (!stream.ok || !closed) return;  // the previous complete dump stays
  std::error_code ec;
  std::filesystem::rename(temporary, g_file, ec);
  if (ec) {  // a file system that does not replace on rename
    std::filesystem::remove(g_file, ec);
    std::filesystem::rename(temporary, g_file, ec);
  }
}

void Start() {
  const std::filesystem::path folder = rex::filesystem::GetExecutableFolder() / "pgo";
  std::error_code ec;
  std::filesystem::create_directories(folder, ec);
  for (int n = 1; n < 100000; ++n) {
    const std::filesystem::path candidate = folder / ("sa2_pgo_" + std::to_string(n) + ".stream");
    std::filesystem::path temporary = candidate;
    temporary += ".tmp";
    if (!std::filesystem::exists(candidate, ec) && !std::filesystem::exists(temporary, ec)) {
      g_file = candidate;
      break;
    }
  }
  if (g_file.empty()) return;
  g_thread = std::thread([] {
    std::unique_lock<std::mutex> lock(g_thread_mutex);
    while (!g_wake.wait_for(lock, kPeriod, [] { return g_stop; })) {
      lock.unlock();
      DumpNow();
      lock.lock();
    }
  });
}

void Stop() {
  {
    std::lock_guard<std::mutex> lock(g_thread_mutex);
    g_stop = true;
  }
  g_wake.notify_all();
  if (g_thread.joinable()) g_thread.join();
  DumpNow();
}

}  // namespace sa2::pgo

#else  // not the instrumented build

namespace sa2::pgo {
void Start() {}
void DumpNow() {}
void Stop() {}
}  // namespace sa2::pgo

#endif
