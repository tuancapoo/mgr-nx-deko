#include <cmath>
#include "../src/me/me_vertex_bytes.h"
#include "../src/me/native/sa2_native_vertices_dedupe.h"
#include <array>
#include <cstdio>
#include <random>
#include <vector>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); return 1; } } while (0)

int main() {
  std::mt19937 random(0x826CA7E8);
  unsigned cases = 0;
  for (size_t bytes : {0u, 4u, 8u, 12u, 16u, 20u, 28u, 32u, 60u, 64u, 68u, 252u,
                       256u, 260u, 1024u, 4096u, 16384u, 65536u}) {
    for (unsigned alignment = 0; alignment < 16; ++alignment) for (unsigned order = 0; order < 4; ++order) {
      std::vector<uint8_t> source(bytes + 32), target(bytes + 32, 0xa7);
      for (auto& b : source) b = uint8_t(random());
      auto* s = source.data() + alignment;
      auto* d = target.data() + (15 - alignment);
      // Independent scalar definition of the Xenos endian permutations.
      for (size_t i = 0; i < bytes; ++i) {
        const unsigned lane = unsigned(i % 4);
        const unsigned from = order == 0 ? lane : order == 1 ? lane ^ 1 : order == 2 ? 3 - lane : lane ^ 2;
        d[i] = s[(i / 4) * 4 + from];
      }
      CHECK(me::native::VertexUploadEquals(s, d, bytes, order));
      if (bytes) {
        for (size_t changed : {size_t(0), bytes / 2, bytes - 1}) {
          d[changed] ^= 1;
          CHECK(!me::native::VertexUploadEquals(s, d, bytes, order));
          d[changed] ^= 1;
        }
      }
      ++cases;
    }
  }
  CHECK(!me::native::VertexUploadEquals(nullptr, nullptr, 0, 4));
  CHECK(!me::native::VertexUploadEquals(nullptr, nullptr, 3, 0));
  sa2::native::DedupeVertices cache;
  cache.NewFrame(1);
  uint64_t offset = 0, comparisons = 0;
  auto equal = [&](uint64_t p) { ++comparisons; return p == 64; };
  CHECK(!cache.SearchExact(0x1000, 1024, 2, offset, equal));
  CHECK(comparisons == 0);  // no memory read or hash on a metadata miss
  cache.Note(0x1000, 1024, 2, 64);
  CHECK(cache.SearchExact(0x1000, 1024, 2, offset, equal) && offset == 64);
  CHECK(!cache.SearchExact(0x1000, 1020, 2, offset, equal));
  CHECK(!cache.SearchExact(0x1000, 1024, 1, offset, equal));
  CHECK(!cache.SearchExact(0x2000, 1024, 2, offset, equal));
  CHECK(comparisons == 1);
  CHECK(!cache.SearchExact(0x1000, 1024, 2, offset, [](uint64_t) { return false; }));
  CHECK(cache.hits() == 1 && cache.discrepancies() == 1 && cache.bytes_saved() == 1024);
  cache.Forget();
  CHECK(!cache.SearchExact(0x1000, 1024, 2, offset, equal));
  cache.Note(0x1000, 1024, 2, 64);
  cache.NewFrame(2);
  CHECK(!cache.SearchExact(0x1000, 1024, 2, offset, equal));
  CHECK(comparisons == 1);
  std::printf("PASS %u endian/alignment/range cases, all-byte mutation rejection and cache miss/epoch guards\n", cases);
}
