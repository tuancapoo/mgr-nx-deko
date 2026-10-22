#include "../src/me/me_stream_conversion.h"
#include <chrono>
#include <cstdio>
#include <vector>
#include <random>
volatile uint32_t sink=0;
__declspec(noinline) void Legacy(const uint8_t* src,uint8_t* dst,unsigned words,unsigned stride,unsigned format,bool sign) {
  for(unsigned i=0;i<words;++i){uint32_t v;std::memcpy(&v,src+i*4,4);v=std::byteswap(v);std::memcpy(dst+i*4,&v,4);}
  for(unsigned at=4;at+4<=words*4;at+=stride){uint32_t v;std::memcpy(&v,dst+at,4);
    const uint32_t widths[]={format==16?11u:10u,11u,format==16?10u:11u};uint32_t out=0,shift=0;
    for(unsigned c=0;c<3;++c){unsigned w=widths[c];uint32_t x=(v>>shift)&((1u<<w)-1);shift+=w;
      if(w==11)x=sign?uint32_t(int32_t(x<<21)>>22)&0x3ff:x>>1;out|=(x&0x3ff)<<(c*10);}
    out|=(sign?1u:3u)<<30;std::memcpy(dst+at,&out,4);}
}
__declspec(noinline) void Fused(const uint8_t* src,uint8_t* dst,unsigned words,const me::native::RepackStreamPlan& plan) {
  me::native::CopyRepacked<2>(src,dst,words,plan);
}
int main(){std::mt19937 rng(7);
  for(unsigned bytes:{1024u,65536u})for(unsigned stride:{16u,32u,48u,64u}){
    std::vector<uint8_t> src(bytes),dst(bytes);for(auto&v:src)v=uint8_t(rng());
    me::native::RepackStreamPlan plan{};plan.Init(stride);plan.Add(4,16,true);
    double times[2]{};
    for(unsigned method=0;method<2;++method){auto start=std::chrono::steady_clock::now();
      for(unsigned i=0;i<500000000u/bytes;++i){if(method)Fused(src.data(),dst.data(),bytes/4,plan);else Legacy(src.data(),dst.data(),bytes/4,stride,16,true);sink=dst[i%bytes];}
      times[method]=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-start).count();}
    std::printf("%u bytes stride %u: old %.0f us fused %.0f us ratio %.2f\n",bytes,stride,times[0],times[1],times[1]/times[0]);
  }
}
