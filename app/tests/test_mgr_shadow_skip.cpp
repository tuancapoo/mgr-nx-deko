#include "../src/me/me_mgr_shadow_skip.h"
#include <cstdio>
#define CHECK(x) do { if (!(x)) {std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);return 1;} } while(0)
int main() {
  using namespace me::native;
  MgrShadowDraw draw{1040,0,0x400,1,5,0,0x00700266,6,369,0,0x04000400,0};
  MgrShadowDraw clear=draw;clear.primitive=8;clear.count=3;clear.depth_control=0x76;
  std::optional<std::array<int32_t,4>> full=std::array<int32_t,4>{0,0,1040,1040};
  MgrShadowSkip state;
  CHECK(MgrShadowGeometry(draw));CHECK(!state.Skip(1,draw));
  CHECK(state.Cleared(1,clear,full,0.0f));CHECK(state.Skip(1,draw));CHECK(state.Skip(1,draw));
  CHECK(!state.Skip(2,draw));
  auto alias=clear;alias.pitch=520;alias.msaa=2;
  CHECK(!state.Cleared(2,alias,std::array<int32_t,4>{0,0,480,512},0.0f));CHECK(!state.Skip(2,draw));
  CHECK(state.Cleared(2,clear,full,0.0f));CHECK(!state.Skip(2,alias));
  CHECK(state.Cleared(2,alias,full,0.0f));CHECK(state.Skip(2,draw));
  CHECK(!state.Cleared(2,clear,full,1.0f));CHECK(!state.Skip(2,draw));
  CHECK(!state.Cleared(2,clear,full,std::nullopt));
  CHECK(!state.Cleared(2,clear,std::nullopt,0.0f));
  CHECK(!state.Cleared(2,clear,std::array<int32_t,4>{0,0,1023,1024},0.0f));
  // Exact observed split clear: right edge at 1x, left side through a 2x2 alias.
  {MgrShadowSkip split;
   CHECK(!split.Cleared(7,clear,std::array<int32_t,4>{960,0,1024,1024},0.0f));
   CHECK(!split.Skip(7,alias));
   CHECK(split.Cleared(7,alias,std::array<int32_t,4>{0,0,480,512},0.0f));
   CHECK(split.Skip(7,draw));CHECK(split.Skip(7,draw));CHECK(!split.Skip(8,draw));}
  // A one-pixel gap, including a sub-cell gap, never proves the map clear.
  for(int right:{960,961}){MgrShadowSkip gap;
   CHECK(!gap.Cleared(9,clear,std::array<int32_t,4>{right+1,0,1024,1024},0.0f));
   CHECK(!gap.Cleared(9,alias,std::array<int32_t,4>{0,0,480,512},0.0f));CHECK(!gap.Skip(9,draw));}
  {MgrShadowSkip interrupted;
   CHECK(!interrupted.Cleared(4,clear,std::array<int32_t,4>{960,0,1024,1024},0.0f));
   CHECK(!interrupted.Skip(4,draw));
   CHECK(!interrupted.Cleared(4,alias,std::array<int32_t,4>{0,0,480,512},0.0f));CHECK(!interrupted.Skip(4,draw));}
  {MgrShadowSkip partial_tile;
   CHECK(!partial_tile.Cleared(4,alias,std::array<int32_t,4>{0,0,512,512},0.0f));
   CHECK(!partial_tile.Skip(4,draw));}
  for(unsigned field=0;field<10;++field){
    auto other=draw;
    switch(field){case 0:other.pitch=1280;break;case 1:other.depth_base=0;break;
      case 2:other.depth_format=0;break;case 3:other.msaa=1;break;
      case 4:other.color_mask=1;break;case 5:other.depth_control=0x62;break;
      case 6:other.primitive=8;break;case 7:other.scissor_br=0x02000200;break;
      case 8:other.scissor_tl=1;break;case 9:other.window_offset=1;break;}
    CHECK(!MgrShadowGeometry(other));
    CHECK(state.Cleared(3,clear,full,0.0f));CHECK(!state.Skip(3,other));
    if(!MgrShadowClearSurface(other))CHECK(!state.Skip(3,draw));
  }
  // Source traced variations: depth-only/color-depth and indexed primitive families.
  for(unsigned mode:{4u,5u})for(unsigned primitive:{4u,5u,6u,13u}){
    auto d=draw;d.mode=mode;d.primitive=primitive;CHECK(MgrShadowGeometry(d));}
  std::puts("PASS MGR shadow surface/geometry guard, zero-clear proof, alias, frame and pass lifetime, scene exclusions");
}
