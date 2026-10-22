#include "../src/me/me_stream_conversion.h"
#include "../src/me/me_scratch_vectors.h"
#include <cstdio>
#include <random>
#include <vector>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); return 1; } } while(0)

uint32_t Swap(uint32_t v, unsigned order) {
  if(order==1) return ((v&0xff00ffu)<<8)|((v>>8)&0xff00ffu);
  if(order==2) return std::byteswap(v);
  if(order==3) return (v<<16)|(v>>16);
  return v;
}
uint32_t LegacyRepack(uint32_t v, unsigned format, bool sign) {
  const unsigned widths[]={format==16?11u:10u,11u,format==16?10u:11u};
  uint32_t out=0,shift=0;
  for(unsigned c=0;c<3;++c){unsigned width=widths[c];uint32_t value=(v>>shift)&((1u<<width)-1);shift+=width;
    if(width==11)value=sign?uint32_t(int32_t(value<<21)>>22)&0x3ffu:value>>1;
    out|=(value&0x3ffu)<<(c*10);}
  return out|((sign?1u:3u)<<30);
}
int main() {
  std::mt19937 random(0x1785921);
  uint64_t cases=0;
  for(unsigned f:{16u,17u})for(bool sign:{false,true}) {
    for(uint32_t v: {0u,~0u,0x80000000u,0x7fffffffu,0xaaaaaaaau,0x55555555u})
      CHECK(me::native::Repack1011(v,f,sign)==LegacyRepack(v,f,sign));
    for(unsigned i=0;i<100000;++i){uint32_t v=random();CHECK(me::native::Repack1011(v,f,sign)==LegacyRepack(v,f,sign));}
  }
  for(unsigned order=0;order<4;++order)for(unsigned n=0;n<257;++n)
    for(uint32_t delta:{0u,1u,65535u,0xffffffu,0xff8000u,0xffff0000u})for(bool restart:{false,true}) {
      const uint32_t reset=(n%3==0)?0xffff:(n%3==1?1234:0x123456);
      std::vector<uint8_t> src(size_t(n+2)*2+3);
      for(unsigned i=0;i<n+2;++i){uint16_t v=i%7?uint16_t(random()):uint16_t(reset);std::memcpy(src.data()+1+i*2,&v,2);}
      std::vector<uint32_t> got(n+4,0xdeadbeef),expected(n);
      uint32_t mn=UINT32_MAX,mx=0,gm=0,gx=0;
      for(unsigned i=0;i<n;++i){uint16_t raw;std::memcpy(&raw,src.data()+1+size_t(order>=2?(i^1):i)*2,2);
        if(order==1||order==2)raw=std::byteswap(raw);
        if(restart&&raw==reset){expected[i]=UINT32_MAX;continue;}
        uint32_t v=(uint32_t(raw)+delta)&0xffffff;expected[i]=v;mn=std::min(mn,v);mx=std::max(mx,v);}
      switch(order){
        case 0:me::native::DecodeIndices16Wide<0>(src.data()+1,n,got.data()+2,delta,restart,reset,gm,gx);break;
        case 1:me::native::DecodeIndices16Wide<1>(src.data()+1,n,got.data()+2,delta,restart,reset,gm,gx);break;
        case 2:me::native::DecodeIndices16Wide<2>(src.data()+1,n,got.data()+2,delta,restart,reset,gm,gx);break;
        case 3:me::native::DecodeIndices16Wide<3>(src.data()+1,n,got.data()+2,delta,restart,reset,gm,gx);break;}
      CHECK(mn==gm&&mx==gx);CHECK(std::equal(expected.begin(),expected.end(),got.begin()+2));
      CHECK(got[0]==0xdeadbeef&&got[1]==0xdeadbeef&&got[n+2]==0xdeadbeef&&got[n+3]==0xdeadbeef);++cases;
    }
  for(unsigned order=0;order<4;++order)for(unsigned stride=16;stride<=256;stride+=16)
    for(unsigned tail=0;tail<stride/4;++tail)for(unsigned alignment=0;alignment<4;++alignment){
      me::native::RepackStreamPlan plan{};CHECK(plan.Init(stride));
      for(unsigned w=0;w<stride/4;w+=3)CHECK(plan.Add(w*4,w%2?16:17,(w%5)==0));
      const unsigned words=stride/4*9+tail;
      std::vector<uint8_t> src(words*4+alignment),got(words*4+6,0xed),expected(words*4);
      for(auto&v:src)v=uint8_t(random());
      for(unsigned i=0;i<words;++i){uint32_t v;std::memcpy(&v,src.data()+alignment+i*4,4);v=Swap(v,order);
        unsigned phase=i%(stride/4);if(plan.active[phase])v=LegacyRepack(v,plan.shift[phase]?16:17,plan.w[phase]==(1u<<30));
        std::memcpy(expected.data()+i*4,&v,4);}
      switch(order){
        case 0:me::native::CopyRepacked<0>(src.data()+alignment,got.data()+3,words,plan);break;
        case 1:me::native::CopyRepacked<1>(src.data()+alignment,got.data()+3,words,plan);break;
        case 2:me::native::CopyRepacked<2>(src.data()+alignment,got.data()+3,words,plan);break;
        case 3:me::native::CopyRepacked<3>(src.data()+alignment,got.data()+3,words,plan);break;}
      CHECK(std::equal(expected.begin(),expected.end(),got.begin()+3));
      // Expected bytes above are produced by the independent scalar oracle.
      // Accept them at every endian/stride/alignment/tail, then reject damage.
      auto equal_repacked = [&] {
        switch (order) {
          case 0:return me::native::RepackedUploadEquals<0>(src.data()+alignment,got.data()+3,words,plan);
          case 1:return me::native::RepackedUploadEquals<1>(src.data()+alignment,got.data()+3,words,plan);
          case 2:return me::native::RepackedUploadEquals<2>(src.data()+alignment,got.data()+3,words,plan);
          default:return me::native::RepackedUploadEquals<3>(src.data()+alignment,got.data()+3,words,plan);
        }
      };
      CHECK(equal_repacked());
      for (unsigned changed : {0u, words*2, words*4-1}) {
        got[3+changed]^=1;CHECK(!equal_repacked());got[3+changed]^=1;
      }
      // A changed source whose converted output changes must reject a stale
      // upload. Toggling every bit of a word guarantees at least one kept bit.
      for(unsigned byte=0;byte<4;++byte)src[alignment+byte]^=255;
      CHECK(!equal_repacked());
      for(unsigned byte=0;byte<4;++byte)src[alignment+byte]^=255;
      CHECK(got[0]==0xed&&got[2]==0xed&&got[words*4+3]==0xed&&got.back()==0xed);++cases;
    }
  me::native::RepackStreamPlan bad{};CHECK(!bad.Init(0));CHECK(!bad.Init(20));CHECK(!bad.Init(272));
  CHECK(bad.Init(32));CHECK(!bad.Add(1,16,false));CHECK(!bad.Add(32,16,false));CHECK(!bad.Add(4,18,false));
  CHECK(bad.Add(4,16,false));CHECK(!bad.Add(4,17,true));
  me::native::ScratchVectors<int,2,128> pool;
  int* retained=nullptr;
  {auto a=pool.Acquire(true);a.get().resize(64);retained=a.get().data();
   {auto b=pool.Acquire(true);b.get().assign(64,2);{auto c=pool.Acquire(true);c.get().assign(512,3);}
    CHECK(b.get().size()==64&&b.get()[0]==2);}
   CHECK(a.get().data()==retained&&a.get().size()==64);}
  {auto a=pool.Acquire(true);CHECK(a.get().empty()&&a.get().data()==retained);a.get().resize(256);}
  {auto a=pool.Acquire(true);CHECK(a.get().empty()&&a.get().capacity()==0);}
  {auto a=pool.Acquire(false);a.get().assign(16,7);}
  std::printf("PASS %llu stream layouts, 400024 packed words, nested/bounded scratch\n",cases);
}
