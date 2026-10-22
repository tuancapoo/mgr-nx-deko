#include "../src/me/me_stream_conversion.h"
#include "../src/me/native/sa2_native_vertices_dedupe.h"
#include <array>
#include <cstdio>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); return 1; } } while(0)
int main() {
  sa2::native::DedupeVertices cache;
  cache.NewFrame(1);
  std::array<uint8_t,64> source{},upload{};
  for(unsigned i=0;i<source.size();++i)source[i]=uint8_t(i*7+1);
  me::native::RepackStreamPlan first{},second{};
  CHECK(first.Init(16)&&first.Add(0,16,true));
  CHECK(second.Init(16)&&second.Add(0,17,false));
  me::native::CopyRepacked<2>(source.data(),upload.data(),16,first);
  unsigned comparisons=0;uint64_t offset=0;
  auto equal_first=[&](uint64_t p){++comparisons;return p==0&&me::native::RepackedUploadEquals<2>(source.data(),upload.data(),16,first);};
  auto equal_second=[&](uint64_t p){return p==0&&me::native::RepackedUploadEquals<2>(source.data(),upload.data(),16,second);};
  CHECK(!cache.SearchExact(0x1000,64,2,offset,equal_first)&&comparisons==0);
  cache.Note(0x1000,64,2,0);
  CHECK(cache.SearchExact(0x1000,64,2,offset,equal_first));
  CHECK(!cache.SearchExact(0x1000,64,2,offset,equal_second)); // same address, different conversion
  source[12]^=1; // unchanged-format word must also participate
  CHECK(!cache.SearchExact(0x1000,64,2,offset,equal_first));
  source[12]^=1;
  cache.Forget();
  const unsigned before=comparisons;
  CHECK(!cache.SearchExact(0x1000,64,2,offset,equal_first)&&comparisons==before);
  cache.Note(0x1000,64,2,0);
  cache.NewFrame(2);
  CHECK(!cache.SearchExact(0x1000,64,2,offset,equal_first)&&comparisons==before);
  std::puts("PASS repacked cache: exact output, layout changes, source mutation, synchronization and slot expiry");
}
