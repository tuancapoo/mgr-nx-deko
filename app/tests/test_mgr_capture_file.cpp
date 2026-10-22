#include "me/me_capture_file.h"
#include <cassert>
#include <iostream>
#include <iterator>
#include <string>

int main(int argc, char** argv) {
  using me::capture::WriteOnce;
  using me::capture::WriteResult;
  assert(argc == 2);
  const std::filesystem::path root(argv[1]);
  assert(!std::filesystem::exists(root));
  const auto path = root / "variants/vs_test.bin";
  const std::string payload(12003, 'a');  // crosses the comparison buffer boundary
  assert(WriteOnce(path, payload.data(), payload.size()) == WriteResult::kWritten);
  const auto time = std::filesystem::last_write_time(path);
  assert(WriteOnce(path, payload.data(), payload.size()) == WriteResult::kPresent);
  assert(std::filesystem::last_write_time(path) == time);
  std::string different = payload;
  different.back() = 'b';
  assert(WriteOnce(path, different.data(), different.size()) == WriteResult::kConflict);
  std::ifstream file(path, std::ios::binary);
  assert(std::string(std::istreambuf_iterator<char>(file), {}) == payload);
  file.close();
  // An interrupted capture cannot block collection in subsequent sessions.
  const auto second = root / "variants/vs_retry.bin";
  std::ofstream(second.string() + ".pending", std::ios::binary) << "partial";
  assert(WriteOnce(second, payload.data(), payload.size()) == WriteResult::kWritten);
  assert(!std::filesystem::exists(second.string() + ".pending"));
  // A destination that cannot be created must remain a failure, not a success.
  assert(WriteOnce(path / "impossible.bin", payload.data(), payload.size()) == WriteResult::kFailed);
  std::cout << "PASS: persistent deduplication, conflict preservation, interrupted-file retry, write errors\n";
}
