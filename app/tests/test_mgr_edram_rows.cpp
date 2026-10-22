#include "../src/me/native/me_edram_import_rows.h"
#include "../src/me/me_immutable_descriptor_cache.h"
#include "../src/me/me_readable_range_cache.h"
#include <array>
#include <cstdio>
#include <random>

#define CHECK(x) do { if (!(x)) { std::printf("FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)
struct Span {
  uintptr_t source;
  uint32_t tile_source, tile, count;
  bool native_msaa_import = false;
  uint32_t row_width = 0;
};
using Contents = std::array<uint64_t, 2048>;
Contents Apply(const std::vector<Span>& spans, uint32_t pitch) {
  Contents out; out.fill(~uint64_t(0));
  for (const auto& s : spans)
    for (uint32_t i = 0; i < s.count; ++i)
      if (me::native::EdramImportRows{pitch, s.row_width}.includes(i))
        out[s.tile + i] = (uint64_t(s.source) << 32) | (s.tile_source + i);
  return out;
}
int main() {
  using namespace me::native;
  uint64_t cases = 0;
  std::vector<Span> atlas;
  for (unsigned y = 0; y < 64; ++y) {
    atlas.push_back({1, y * 13, y * 13, 12});
    atlas.push_back({2, y * 13 + 12, y * 13 + 12, 1});
  }
  const auto original = atlas;
  const auto expected = Apply(atlas, 13);
  CHECK(CoalesceEdramImportRows(atlas, 13, [](const auto& s) { return s.source == 1; }) == 63);
  CHECK(atlas.size() == 65 && atlas[0].row_width == 12 && atlas[0].count == 831);
  CHECK(Apply(atlas, 13) == expected);
  // Model the existing shader: scissor removes the holes while a linear tile
  // range remaps each remaining fragment to exactly its original source tile.
  for (unsigned q = 1; q <= 4; ++q) for (unsigned mx = 0; mx < 2; ++mx)
    for (unsigned my = 0; my < 2; ++my) {
      const unsigned tw = (20 * q) >> mx, th = (4 * q) >> my;
      std::vector<uint64_t> a(13 * tw * 64 * th, ~uint64_t(0)), b = a;
      auto raster = [&](const std::vector<Span>& runs, auto& image) {
        for (const auto& s : runs) {
          const unsigned first_y = s.tile / 13, last_y = (s.tile + s.count - 1) / 13;
          const unsigned x0 = (s.row_width || first_y == last_y) ? s.tile % 13 : 0;
          const unsigned x1 = (s.row_width || first_y == last_y) ? (s.tile+s.count-1)%13+1 : 13;
          for (unsigned y = first_y*th; y < (last_y+1)*th; ++y)
            for (unsigned x = x0*tw; x < x1*tw; ++x) {
              const unsigned tile = (y/th)*13+x/tw;
              if (tile < s.tile || tile-s.tile >= s.count) continue;
              image[y*13*tw+x] = (uint64_t(s.source)<<48) |
                  (uint64_t(s.tile_source+tile-s.tile)<<20) | ((y%th)<<10) | (x%tw);
            }
        }
      };
      raster(original, a); raster(atlas, b); CHECK(a == b); ++cases;
    }
  std::mt19937 rng(0x1040720);
  for (unsigned round = 0; round < 30000; ++round) {
    const uint32_t pitch = 1 + rng()%128;
    const uint32_t rows = 1 + rng()%(2048/pitch);
    std::vector<Span> runs;
    for (unsigned y = 0; y < rows; ++y) {
      for (unsigned x = 0; x < pitch;) {
        unsigned width = 1 + rng()%(pitch-x);
        unsigned source = 1 + rng()%4;
        if (rng()%4) runs.push_back({source, y*pitch+x, y*pitch+x, width, source == 4});
        x += width;
      }
    }
    auto before = Apply(runs, pitch);
    CoalesceEdramImportRows(runs, pitch, [](const auto& s) { return !s.native_msaa_import; });
    CHECK(Apply(runs, pitch) == before);
    for (const auto& s : runs) {
      CHECK((EdramImportRows{pitch,s.row_width}.valid(s.tile,s.count)));
      CHECK((EdramImportRows{pitch,s.row_width}.valid(s.tile_source,s.count)));
    }
    ++cases;
  }
  CHECK((!EdramImportRows{13,12}.valid(2,12)));
  CHECK((!EdramImportRows{13,12}.valid(0,24)));
  CHECK((!EdramImportRows{13,12}.valid(2040,25)));
  CHECK((!EdramImportRows{0,12}.valid(0,12)));
  CHECK((!EdramImportRows{13,12}.includes(12)));

  using Key = std::array<uint64_t,23>;
  ImmutableDescriptorCache<Key,uint64_t,32> cache;
  for (unsigned recording = 1; recording < 10001; ++recording) {
    Key key{}; key[0] = 17; key[1] = 3;
    CHECK(!cache.Find(key));
    cache.Remember(key, recording*100);
    cache.Remember(key, recording*100+1);  // live contents cannot be replaced
    CHECK(cache.Find(key) == recording*100);
    for (unsigned field = 0; field < key.size(); ++field) {
      Key changed = key; ++changed[field]; CHECK(!cache.Find(changed));
      cache.Remember(changed, recording*100+field+2);
    }
    CHECK(cache.size() == 24);
    cache.ClearAfterFence();
  }
  for (unsigned i=1;i<100;++i) { Key k{};k[0]=i;cache.Remember(k,i); }
  CHECK(cache.size()==32);
  Key overflow{};overflow[0]=99;CHECK(!cache.Find(overflow));

  unsigned queries = 0;
  std::array<bool,128> readable{}; readable.fill(true);
  auto query = [&](uint32_t start, uint32_t bytes) {
    ++queries;
    if (start%4096 || bytes%4096 || start/4096+bytes/4096 > readable.size()) return false;
    for (unsigned p=start/4096;p<(start+bytes)/4096;++p) if (!readable[p]) return false;
    return true;
  };
  ReadableRangeCache permissions;
  for (unsigned i=0;i<64;++i) CHECK(permissions.Check(0x1000+i*16,16,query));
  CHECK(queries==1);
  readable[2]=false;
  CHECK(!permissions.Check(0x1ffc,8,query));  // next page is unreadable
  CHECK(permissions.Check(0x1ffc,4,query));
  CHECK(!permissions.Check(0x1ffffffc,8,query));
  CHECK(!permissions.Check(0,0,query));
  readable[1]=false;
  ReadableRangeCache next_draw;
  CHECK(!next_draw.Check(0x1000,16,query));  // permissions never escape a draw
  for (unsigned round=0;round<10000;++round) {
    ReadableRangeCache local;
    for (auto&& p:readable) p=rng()%4!=0;
    for (unsigned n=0;n<40;++n) {
      uint32_t address=rng()%(128*4096),length=1+rng()%8192;
      bool expected=address+length<=128*4096;
      if(expected)for(unsigned p=address/4096;p<=(address+length-1)/4096;++p)expected &= readable[p];
      CHECK(local.Check(address,length,query)==expected);
    }
  }
  std::printf("PASS %llu import/pixel cases, 10000 descriptor recordings, 400000 permission queries\n",
              static_cast<unsigned long long>(cases));
}
