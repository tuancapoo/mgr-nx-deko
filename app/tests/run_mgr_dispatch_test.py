"""Execute the real generated MGR dispatcher with recording callees and the original PPC tables."""
from pathlib import Path
import re, struct, subprocess, argparse, sys
root=Path(__file__).resolve().parents[2]
ap=argparse.ArgumentParser()
ap.add_argument('--output',type=Path,default=root/'out/experiments/crash_renderer_20261016/dispatch_test')
args=ap.parse_args();args.output.mkdir(parents=True,exist_ok=True)
sys.path.insert(0,str(root/'tools'))
from mgr_repair_branches import repair
image=(root/'out/default_image.bin').read_bytes()
def extract(text):return text.split('DEFINE_REX_FUNC(sub_82EEEEF0) {',1)[1].split('\n}\n',1)[0]
text=(root/'app/generated/default/sa2_recomp.3.cpp').read_text()
assert repair(text,image)[1]==0
old=(root/'out/experiments/full_codegen_20261016/generated_pass1/default/sa2_recomp.3.cpp').read_text()
repaired,n=repair(old,image);assert n==18
assert repair(repaired,image)[1]==0
bad=bytearray(image);bad[0x82EEF494-0x82000000]^=1
try:repair(old,bad)
except ValueError:pass
else:raise AssertionError('Mismatching PPC table was accepted')
def branch(addr):
 w=struct.unpack_from('>I',image,addr-0x82000000)[0];assert w>>26==18 and not w&3
 d=w&0x3fffffc
 if d&0x2000000:d-=0x4000000
 return (addr+d)&0xffffffff
cases=[(0x100001+i,branch(0x82EEF4D0+4*i)) for i in range(15)]
cases += [(0x200000,branch(0x82EEF50C)),(0x200001,branch(0x82EEF544))]
cases += [(0x200002+i,branch(0x82EEF548+4*i)) for i in range(7)]
for tag,body in [('fixed',extract(text)),('negative',extract(old))]:
 callees=sorted(set(re.findall(r'\b((?:__imp__)?sub_[0-9A-F]{8})\(ctx, base\)',body)))
 src='''#include <cstdint>
#include <cstring>
#include <cstdio>
#include <stdexcept>
#include <fstream>
#include <vector>
union PPCRegister { uint64_t u64; int64_t s64; uint32_t u32; int32_t s32; };
struct PPCXERRegister {};
struct PPCCRRegister { bool lt=false,gt=false,eq=false; template<class T,class A,class B> void compare(A a,B b,PPCXERRegister&) {lt=T(a)<T(b);gt=T(a)>T(b);eq=T(a)==T(b);} };
struct PPCContext {'''+''.join(f'PPCRegister r{i}{{}};' for i in range(32))+'''};
static std::vector<uint8_t> image; static uint32_t kind,selected;
static uint32_t Read32(uint32_t a){ if(a==0x1000+1544)return kind; if(a<0x82000000 || uint64_t(a)-0x82000000+4>image.size())throw std::runtime_error("bad read"); size_t i=a-0x82000000;return uint32_t(image[i])<<24|uint32_t(image[i+1])<<16|uint32_t(image[i+2])<<8|image[i+3]; }
#define REX_FUNC_PROLOGUE() ((void)0)
#define REX_LOAD_U32(a) Read32(uint32_t(a))
#define REX_LOAD_D_U32(a,d) Read32(uint32_t(a)+uint32_t(d))
#define REX_FATAL(...) throw std::runtime_error("unresolved branch")
#define REX_LOOP_BARRIER() ((void)0)
'''
 src+='\n'.join(f'void {n}(PPCContext&,uint8_t*){{selected=0x{n[-8:]}u;}}' for n in callees)
 src+='\nvoid Dispatch(PPCContext& ctx,uint8_t* base){'+body+'\n}\n'
 src+='int main(int argc,char**argv){std::ifstream f(argv[1],std::ios::binary); image.assign(std::istreambuf_iterator<char>(f),{}); unsigned failed=0;'
 for selector,target in cases:
  src+='{PPCContext c{};c.r3.u64=0x1000;c.r4.u64=0x123456789abcdef0ULL;c.r5.u64=0xfedcba9876543210ULL;kind=0x%x;selected=0;try{Dispatch(c,nullptr);}catch(...){++failed;}if(selected!=0x%x || c.r3.u64!=0x1000 || c.r4.u64!=0x123456789abcdef0ULL || c.r5.u64!=0xfedcba9876543210ULL)++failed;}'%(selector,target)
 src+=f'std::printf("{tag}: {len(cases)} original-PPC dispatch destinations, %u failures\\n",failed);return failed?1:0;}}'
 source=args.output/(tag+'.cpp');source.write_text(src)
 exe=args.output/(tag+'.exe')
 subprocess.run(['clang++','-std=c++23','-O2',str(source),'-o',str(exe)],check=True)
 result=subprocess.run([str(exe),str(root/'out/default_image.bin')])
 assert result.returncode==(0 if tag=='fixed' else 1)
print('PASS actual dispatcher, all 15 table cases and 9 conditional-tail paths; old-code negative control and image-mismatch/idempotence guards')
