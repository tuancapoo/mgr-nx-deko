# Installer probe (2026-10-23)

Can an installer rebuild MGR's shader package from the player's own files without running the game?

- `cpk_toc.py <file.cpk>`: CPK table of contents (@UTF tables, masked or not).
- `cpk_shader_scan.cpp`: `cpkscan <cpk> <list.txt> <outdir>` — reads each entry ("offset size extract name"
  lines, from cpk_toc.toc()), undoes CRILAYLA, writes every Xenos shader container (magic 0x102A1100/01, header and
  microcode sizes) it finds. Build: `clang++ -O2 -std=c++17 -D_CRT_SECURE_NO_WARNINGS cpk_shader_scan.cpp`.

Result against the 1,119 containers the current package was made from (runtime dumps, 1,197 files):
- `media/data000.cpk`: 1,540 containers in 2,995 entries, 1,202 unique; **1,036 of the 1,119 byte for byte** (and
  166 the dumps never met). `data001.cpk`: none. `default.xex` image: no container.
- 80 of the other 83 are runtime variants: Xbox D3D rewrites vertex-fetch instructions of a vertex shader for the
  bound declaration, so the dumped bytes differ from a cpk original of the same length by 64 bytes at most
  (tools/repair_mgr_shader_variants.py in mgr-nx names them proven/repaired/bridge).
- 3 vertex shaders (1436E54461367985, 6D38CFA50AA12E5F, E35EA97ADB2AB935): microcode stored raw in default.xex
  (0x8213AE10, 0x8213B4E8, 0x8213B5F0); the 520-byte container header is built at run time.

## Do the runtime variants matter for reNut? (2026-10-23)
No. reNut keys a vertex shader by the hash of the container the game hands to XGRegisterVertexShader
(d3d/hooks/shader.cpp, RegisterGuestShader), before D3DVertexShader_Bind patches any vertex fetch, and its
translated shaders read their inputs from the bound declaration (ShaderVertexInput fetch 95), not from the fetch
words. The 80 proven/repaired/bridge variants came from the native (PM4) renderer of mgr-nx, which saw microcode
after the bind. In every PC draw log of mgr-nx-deko (34 distinct vertex shaders) none of the 80 appears.

The 3 vertex shaders with microcode in default.xex sit beside Direct3D's own strings (its stereoscopic front-buffer
warning): Direct3D's internal shaders, their container (constant table g_Color, g_HalfPixel, g_ViewProjMatrix, ...)
built by Direct3D at start. No PC draw log uses them (reNut does clears and resolves itself).

So an installer needs only the containers of media/data000.cpk (1,202, a superset of the 1,036 originals played so
far); the 83 others can leave the release package. To confirm in play: a run of every chapter with the cpk-only
package and no "[shaders] missing"/CaptureMissingShader line.
