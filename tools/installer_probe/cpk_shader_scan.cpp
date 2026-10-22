// Usage: cpkscan <cpk> <list.txt> <outdir>   list: "offset size extract name" per line
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
static std::vector<uint8_t> crilayla(const std::vector<uint8_t>& in) {
  uint32_t usize, hoff; memcpy(&usize, &in[8], 4); memcpy(&hoff, &in[12], 4);
  std::vector<uint8_t> out(usize + 0x100);
  memcpy(out.data(), &in[hoff + 0x10], 0x100);
  long ip = (long)in.size() - 0x100 - 1;
  const long oend = 0x100 + (long)usize - 1;
  uint32_t pool = 0; int left = 0; long done = 0;
  auto bits = [&](int n) { uint32_t v = 0; int got = 0;
    while (got < n) { if (!left) { pool = in[ip--]; left = 8; }
      int t = left < n - got ? left : n - got;
      v = (v << t) | ((pool >> (left - t)) & ((1u << t) - 1)); left -= t; got += t; }
    return v; };
  const int vle[4] = {2, 3, 5, 8};
  while (done < (long)usize) {
    if (bits(1)) {
      long back = oend - done + bits(13) + 3; long len = 3; int lv;
      for (lv = 0; lv < 4; ++lv) { uint32_t t = bits(vle[lv]); len += t; if (t != (1u << vle[lv]) - 1) break; }
      if (lv == 4) { uint32_t t; do { t = bits(8); len += t; } while (t == 255); }
      for (long i = 0; i < len && done < (long)usize; ++i) { out[oend - done] = out[back--]; ++done; }
    } else { out[oend - done] = (uint8_t)bits(8); ++done; }
  }
  return out;
}
static uint32_t be(const uint8_t* p) { return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
int main(int c, char** v) {
  FILE* f = fopen(v[1], "rb"); FILE* l = fopen(v[2], "r"); std::string outdir = v[3];
  char name[1024]; unsigned long long off, size, ext; long files = 0, found = 0;
  FILE* idx = fopen((outdir + "/index.txt").c_str(), "w");
  while (fscanf(l, "%llu %llu %llu %1023s", &off, &size, &ext, name) == 4) {
    if (!size) continue;
    std::vector<uint8_t> b(size); _fseeki64(f, off, SEEK_SET); fread(b.data(), 1, size, f);
    if (size > 16 && !memcmp(b.data(), "CRILAYLA", 8)) b = crilayla(b);
    ++files;
    for (size_t i = 0; i + 12 <= b.size(); ++i) {
      if (b[i] != 0x10 || b[i + 1] != 0x2A || b[i + 2] != 0x11 || (b[i + 3] & 0xFE) != 0) continue;
      uint32_t h = be(&b[i + 4]), u = be(&b[i + 8]);
      if (h < 12 || h >= (1u << 20) || !u || u >= (1u << 20) || i + h + u > b.size()) continue;
      char fn[64]; snprintf(fn, sizeof fn, "/c%06ld.bin", found);
      FILE* o = fopen((outdir + fn).c_str(), "wb"); fwrite(&b[i], 1, h + u, o); fclose(o);
      fprintf(idx, "c%06ld.bin %s %zu %u\n", found, name, i, h + u); ++found;
    }
  }
  fclose(idx); printf("%ld files scanned, %ld containers\n", files, found); return 0;
}
