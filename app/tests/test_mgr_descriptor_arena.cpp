#include "../src/me/me_descriptor_arena.h"
#include <array>
#include <cstdio>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); return 1; } } while(0)
int main() {
  me::native::DescriptorArena<unsigned,4> arena;
  unsigned next=0,calls=0;
  auto allocate=[&]{++calls;return ++next;};
  std::array<unsigned,4> first{};
  for(unsigned i=0;i<4;++i){first[i]=arena.Acquire(allocate);CHECK(first[i]==i+1);}
  CHECK(arena.Acquire(allocate)==0&&calls==4); // no reuse in a live recording
  CHECK(arena.used()==4&&arena.allocated()==4);
  for(unsigned recording=0;recording<10000;++recording){
    arena.Rewind(); // caller has completed the simulated fence/reset
    for(unsigned i=0;i<4;++i)CHECK(arena.Acquire(allocate)==first[i]);
    CHECK(arena.Acquire(allocate)==0&&calls==4);
  }
  me::native::DescriptorArena<unsigned,4> growing;
  CHECK(growing.Acquire([]{return 0u;})==0);
  CHECK(growing.used()==0&&growing.allocated()==0);
  CHECK(growing.Acquire(allocate)!=0);
  growing.Rewind();
  CHECK(growing.Acquire([]{return 0u;})!=0); // retained handles need no allocator
  CHECK(growing.Acquire([]{return 0u;})==0);
  CHECK(growing.used()==1&&growing.allocated()==1);
  CHECK(growing.Acquire(allocate)!=0&&growing.used()==2);
  std::puts("PASS bounded descriptor lifetime, unique live uses, 10000 fenced replays and allocation failure recovery");
}
