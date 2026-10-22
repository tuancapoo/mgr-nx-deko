// Sonic Adventure 2: the ReXGlue application object (paths, GPU plugin choice, analysis aids).

#pragma once

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <system_error>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>

#include "cpu/sa2_enhancements.h"
#include "me/me_native_system.h"
#include "me/sa2_render_mode.h"
#include "profiler/sa2_cpu_profile_pc.h"
#include "sa2_capture.h"
#include "sa2_dlc.h"
#include "sa2_language.h"
#include "sa2_pgo.h"

#if defined(SA2_BUILTIN_CONFIG)
#include "sa2_builtin_config.h"  // generated from app/sa2_switch_defaults.toml (CMake)
#endif

class Sa2App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Sa2App>(new Sa2App(ctx, "sa2", PPCImageConfig));
  }

  // The Switch build's fixed settings (app/sa2_switch_defaults.toml, compiled in): applied before sa2.toml, which
  // holds only the player's options (and may still override any of these).
  std::string_view BuiltInConfig() const override {
#if defined(SA2_BUILTIN_CONFIG)
    return kSa2BuiltInConfig;
#else
    return {};
#endif
  }
  // Without an sa2.toml next to the NRO, the player's default one is written there (app/sa2_switch.toml).
  std::string_view DefaultConfigFile() const override {
#if defined(SA2_BUILTIN_CONFIG)
    return kSa2PlayerConfig;
#else
    return {};
#endif
  }

  void OnPostInitLogging() override {
    // The runtime defaults to no GPU plugin: select ReXGlue's Xenos implementation, keeping an explicit
    // command-line or config-file choice.
    if (!rex::cvar::HasNonDefaultValue("gpu_plugin")) {
      rex::cvar::SetFlagByName("gpu_plugin", "xenos");
    }
    // Which enhancements run (own key + group master), in one line; before any guest thread exists.
    sa2::enh::Resolve();
    // The game's language (sa2.toml "language"; auto = the system language): before the game asks for it.
    sa2::language::Apply();
  }

  // Without --game_data_root, use a game_root/ folder next to the executable (on the Switch:
  // sdmc:/switch/sa2-nx/game_root), and on a PC build tree the repository's assets/game_root.
  // An explicit path always wins.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    if (!paths.game_data_root.empty()) {
      return;
    }
    std::error_code ec;
    const auto folder = rex::filesystem::GetExecutableFolder();
    if (folder.empty()) {
      return;
    }
    for (auto candidate = folder; !candidate.empty(); candidate = candidate.parent_path()) {
      for (const auto& sub : {std::filesystem::path("game_root"),
                              std::filesystem::path("assets") / "game_root"}) {
        // The folder itself, or only its asset cache (game_root.nxpack + .nxidx, see sdk pack_device.h).
        auto pack = candidate / sub;
        pack += ".nxpack";
        if (std::filesystem::is_regular_file(candidate / sub / "default.xex", ec) ||
            std::filesystem::is_regular_file(pack, ec)) {
          paths.game_data_root = candidate / sub;
          return;
        }
      }
      if (candidate == candidate.parent_path()) {
        break;
      }
    }
  }

  // Stage 2: with sa2_renderer_native = true the graphics system is the app's own native Vulkan renderer
  // (src/me), and the emulated Xenos GPU plugin is not loaded (ReXApp only loads it when config.graphics is empty).
  void OnPreSetup(rex::RuntimeConfig& config) override {
    sa2::enh::ApplyGpuBoost();
    if (me::native::Enabled()) {
      config.graphics = me::native::CreateGraphicsSystem();
      REXLOG_INFO("graphics: native Vulkan renderer (PM4 ring)");
    }
  }

  // Video recording of the game image for testing (sa2_video_s = N seconds after start; 0 = off). Taken from the
  // presenter's guest output, so it works minimized and nothing can cover it.
  void OnPostSetup() override {
    // PC only: the sampling CPU profiler (sa2_cpu_profile_from_s) and the stack dump (sa2_stack_dump_s).
    nfsmw::perfil_pc::Arrancar();
    // Every 10 s: how often each active enhancement was used (a log shows they really ran).
    sa2::enh::StartReport();
    // DLC packages from dlc/ next to the executable go into the content store before the game starts looking.
    sa2::dlc::InstallPackages(runtime());
    // PC: the key that simulates docking / undocking (render resolution, bind_sa2_docked = F9).
    sa2::render::RegisterKeys();
    sa2::capture::Start([this]() -> rex::ui::Presenter* {
      const auto* rt = runtime();
      const auto* graphics = rt ? rt->graphics_system() : nullptr;
      return graphics ? graphics->presenter() : nullptr;
    });
    // The PGO-instrumented build (SA2_PGO=generate) saves its counts to pgo/ every 2 minutes (nothing otherwise).
    sa2::pgo::Start();
  }

  // The PGO-instrumented build saves its counts when the game loses focus (HOME menu): it may be closed from there.
  void OnWindowFocusChanged(bool focused) override {
    if (!focused) sa2::pgo::DumpNow();
  }

  void OnShutdown() override {
    nfsmw::perfil_pc::Parar();
    sa2::capture::Stop();
    sa2::pgo::Stop();
  }

  // Analysis aid: with SA2_DUMP_IMAGE=<file>, write the loaded guest image (image_base..image_base+image_size) so
  // code pointers stored in data can be scanned for (tools/pointer_scan.py). Does nothing unless the variable is set.
  void OnPostLoadXexImage() override {
    if (!runtime() || !runtime()->memory()) {
      return;
    }
    const char* path = std::getenv("SA2_DUMP_IMAGE");
    if (!path) {
      return;
    }
    const uint8_t* image =
        runtime()->memory()->TranslateVirtual<const uint8_t*>(PPCImageConfig.image_base);
    if (std::FILE* f = std::fopen(path, "wb")) {
      std::fwrite(image, 1, PPCImageConfig.image_size, f);
      std::fclose(f);
      REXLOG_INFO("guest image written to {}", path);
    }
  }
};
