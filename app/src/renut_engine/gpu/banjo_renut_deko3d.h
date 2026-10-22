// banjo-nx (2026-10-17): Renut's renderer on plume's deko3d back end (renut_gpu_backend = "deko3d",
// docs/deko3d-renderer.md). The game shaders' GLSL package in the NRO (tools/deko3d_shader_package.py) and the DKSH
// cache of compiled variants on the SD card.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace plume {
struct RenderDevice;
struct RenderShader;
}  // namespace plume

namespace banjo::renut_gpu::deko3d {

// Whether the NRO carries the GLSL package (Switch builds made with one).
bool PackageAvailable();

// sa2-nx (2026-10-20, 1.0.3.1): whether the GLSL packages in use draw right above 1x (made without
// tools/deko3d_glsl_opt.py's pass scale1, as the installer page makes them from 1.0.3.1 on). Read once (logged when
// not); render/surfaces.cpp keeps 1280x720 otherwise.
bool PackageScales();

// The deko3d shader (plume::Deko3DShaderBlob) of a game shader by its container hash, or null (logged once).
std::unique_ptr<plume::RenderShader> CreateGameShader(plume::RenderDevice *device, uint64_t hash);

// The boolean specialization constants (ids 1000..1255) a game shader has, as ShaderBoolSpecMask gives them.
bool BoolSpecMask(uint64_t hash, uint32_t mask[8]);

// peggle2-nx (2026-10-20): how many game shaders of the shader table have no GLSL in any package (the NRO's, or
// deko3d_glsl.bin beside it): a build without translated game shaders before the installer's file is there.
size_t MissingGameShaders();

// The DKSH cache next to the NRO (deko3d_shaders.bin): what it holds is read at start; variants the console compiles
// are added to it (written by Save, and by a periodic save while the game runs). Installed into plume before the
// device is made.
void InstallShaderCache();

// mgr-nx-deko (2026-10-22): the startup preparation. Every game shader of the GLSL package with its default variant
// loaded on 'device' (a deko3d one), compiled first where the DKSH cache lacks it (and added to the cache), so no draw
// compiles a shader later. 'progress' after each shader. false on another back end (nothing done).
struct PrewarmProgress {
  uint32_t done = 0, total = 0, compiled = 0, failed = 0;
};
bool PrewarmGameShaders(plume::RenderDevice *device, const std::function<void(const PrewarmProgress &)> &progress);
void SaveShaderCache();

// One line for the renderer's report: compiles, cache hits, memory.
std::string Report();

}  // namespace banjo::renut_gpu::deko3d
