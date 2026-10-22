#include "../src/me/me_restart_indices.h"
#include "../src/me/me_constant_history.h"
#include <array>
#include <cstdio>
#include <random>
#include <vector>
#include <limits>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); return 1; } } while(0)
int main() {
  std::mt19937 random(0x82eef490);
  uint64_t cases = 0;
  std::vector<uint32_t> lengths;
  for (uint32_t n = 0; n < 4097; n += n < 32 ? 1 : 31) lengths.push_back(n);
  for (uint32_t n : {8191u,8192u,8193u,16384u,65535u}) lengths.push_back(n);
  for (uint32_t n : lengths) {
    for (bool swap : {false,true}) for (unsigned alignment=0;alignment<4;++alignment) {
      std::vector<uint8_t> source(size_t(n)*2+alignment+16,0xa7);
      std::vector<uint16_t> got(n+16,0x3a5c);
      std::vector<uint32_t> original(n);
      uint32_t mn=UINT32_MAX,mx=0;
      for (uint32_t i=0;i<n;++i) {
        const uint16_t v=i%7 ? uint16_t(random()) : 0xffff;
        const uint16_t encoded=swap ? std::byteswap(v) : v;
        std::memcpy(source.data()+alignment+size_t(i)*2,&encoded,2);
        // Independent legacy UINT32 path, including restart exclusion.
        original[i]=v==0xffff ? UINT32_MAX : v;
        if(v!=0xffff){mn=std::min(mn,uint32_t(v));mx=std::max(mx,uint32_t(v));}
      }
      uint32_t gm=0,gx=0;
      me::native::DecodeRestart16(source.data()+alignment,n,got.data()+4,swap,gm,gx);
      CHECK(gm==mn && gx==mx);
      for(uint32_t i=0;i<n;++i) CHECK((got[i+4]==0xffff ? UINT32_MAX : uint32_t(got[i+4]))==original[i]);
      for(unsigned i=0;i<4;++i) CHECK(got[i]==0x3a5c);
      for(size_t i=n+4;i<got.size();++i) CHECK(got[i]==0x3a5c);
      ++cases;
    }
  }
  for(unsigned n: {1u,7u,8u,9u,16u,65u}) {
    std::vector<uint16_t> input(n,0xffff),output(n);
    uint32_t mn=0,mx=1;
    me::native::DecodeRestart16(reinterpret_cast<uint8_t*>(input.data()),n,output.data(),false,mn,mx);
    CHECK(mn==UINT32_MAX && mx==0 && input==output);
  }
  me::native::ConstantUploadHistory history;
  for(unsigned n: {0u,1u,8u,15u,16u,17u,31u,48u,64u,1024u}) {
    std::vector<uint8_t> bytes(n+4,0x65);
    const auto key=me::native::ConstantHistorySample(bytes.data(),n);
    CHECK(key[0]==n && key==me::native::ConstantHistorySample(bytes.data(),n));
  }
  // Changes outside the sampled ranges can share a lookup key. Full equality
  // below is what prevents reuse in that case, just as for a hash collision.
  std::array<uint8_t,256> collision_a{},collision_b{};
  collision_b[40]=1;
  CHECK(me::native::ConstantHistorySample(collision_a.data(),256)==
        me::native::ConstantHistorySample(collision_b.data(),256));
  history.Record(256,9,1,0);
  uint64_t ignored=0;
  CHECK(!history.Find(collision_b.data(),256,9,1,collision_a.data(),256,ignored));
  std::array<uint8_t,256> upload{};
  std::array<uint8_t,32> a{}, b{};
  a.fill(0x34);b.fill(0x95);
  std::memcpy(upload.data()+64,a.data(),a.size());
  uint64_t offset=999;
  CHECK(!history.Find(a.data(),32,7,1,upload.data(),256,offset));
  history.Record(32,7,1,64);
  CHECK(history.Find(a.data(),32,7,1,upload.data(),256,offset) && offset==64);
  CHECK(!history.Find(a.data(),32,7,1,upload.data(),256,offset,64));
  // Deliberate hash collision must not reuse another byte sequence.
  CHECK(!history.Find(b.data(),32,7,1,upload.data(),256,offset));
  CHECK(!history.Find(a.data(),16,7,1,upload.data(),256,offset));
  CHECK(!history.Find(a.data(),32,7,2,upload.data(),256,offset));
  CHECK(!history.Find(a.data(),32,7,1,upload.data(),80,offset));
  CHECK(!history.Find(a.data(),32,7,1,nullptr,256,offset));
  history.Record(32,7,1,UINT64_MAX-8);
  CHECK(!history.Find(a.data(),32,7,1,upload.data(),256,offset));
  // A/B/A upload sequence and direct-map eviction.
  history.Record(32,7,3,64);
  std::memcpy(upload.data()+128,b.data(),32);
  history.Record(32,8,3,128);
  CHECK(history.Find(a.data(),32,7,3,upload.data(),256,offset) && offset==64);
  CHECK(history.Find(b.data(),32,8,3,upload.data(),256,offset) && offset==128);
  history.Record(32,7+128,3,128);
  CHECK(!history.Find(a.data(),32,7,3,upload.data(),256,offset));
  // Same address recycled by the ring is invalid in a new epoch.
  CHECK(!history.Find(b.data(),32,7+128,4,upload.data(),256,offset));
  std::printf("PASS %llu restart-index layouts vs legacy32; restart-only, tails, alignment; constant history bytes/collisions/epoch/bounds\n",
              static_cast<unsigned long long>(cases));
}
