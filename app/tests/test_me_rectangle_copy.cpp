#include "../src/me/native/me_rectangle_copy.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <vector>

int main() {
  uint32_t rng = 0x362781ab;
  const auto random = [&]() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; };
  for (unsigned trial = 0; trial < 2000; ++trial) {
    const size_t stride = 1 + random() % 256;
    const uint32_t n = 3 + random() % 254, rectangles = 1 + random() % 64;
    const uint32_t vmin = random() % 1000;
    std::vector<uint8_t> source(n * stride);
    for (auto& b : source) b = uint8_t(random());
    std::vector<uint32_t> indices(rectangles * 3);
    for (auto& i : indices) i = vmin + random() % n;
    const size_t bytes = size_t(rectangles) * 18 * stride;
    std::vector<uint8_t> reference(bytes + 32, 0xa7), actual(bytes + 32, 0x5e);
    for (uint32_t q = 0; q < rectangles; ++q)
      for (uint32_t c = 0; c < 3; ++c)
        for (uint32_t v = 0; v < 6; ++v)
          std::memcpy(reference.data() + 16 + (size_t(q)*18 + v*3 + c)*stride,
                      source.data() + size_t(indices[q*3+c]-vmin)*stride, stride);
    auto index = [&](uint32_t i) { return indices[i]; };
    if (!me::native::ExpandRectangleVertexBytes(source.data(), source.size(), stride,
          vmin, rectangles, index, actual.data()+16, bytes) ||
        !std::equal(reference.begin()+16, reference.end()-16, actual.begin()+16) ||
        !std::all_of(actual.begin(), actual.begin()+16, [](uint8_t b) { return b == 0x5e; }) ||
        !std::all_of(actual.end()-16, actual.end(), [](uint8_t b) { return b == 0x5e; })) return 1;
    if (me::native::ExpandRectangleVertexBytes(source.data(), source.size(), stride,
          vmin, rectangles, index, actual.data()+16, bytes-1)) return 2;
    indices.back() = vmin + n;
    if (me::native::ExpandRectangleVertexBytes(source.data(), source.size(), stride,
          vmin, rectangles, index, actual.data()+16, bytes)) return 3;
    indices.back() = vmin ? vmin-1 : UINT32_MAX;
    if (me::native::ExpandRectangleVertexBytes(source.data(), source.size(), stride,
          vmin, rectangles, index, actual.data()+16, bytes)) return 4;
  }
  std::puts("PASS rectangle expansion: 2000 indexed/repeated-corner cases, exact bytes, bounds and guards");
}
