// Metal Gear Rising: Revengeance (Xbox 360): application entry point, stage 1 (PC, the SDK's emulated Xenos GPU).
//
// The project is still named "sa2" for the generated files (tools/codegen.ps1 and generated/rexglue.cmake use that
// prefix); the app class below is the only code of this port so far. Game-specific hooks (Direct3D, hot functions)
// come after the first boot, each with its evidence.

#include "generated/default/sa2_init.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <functional>
#include <chrono>
#include <thread>
#include <optional>
#include <stdexcept>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/perf/counter.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>
#include <rex/ui/keybinds.h>
#include <rex/input/input_system.h>
#include <rex/system/xthread.h>

#if defined(MGR_NATIVE_RENDERER) || defined(MGR_RENUT_RENDERER)
#include "cpu/sa2_enhancements.h"
#include "me/sa2_render_mode.h"
#endif

REXCVAR_DEFINE_BOOL(mgr_shared_input, true, "MGR input",
                    "Route all controllers to player 1 for MGR's single signed-in profile")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

#if defined(MGR_RENUT_RENDERER)
#include "renut_engine/gpu/banjo_renut_select.h"
#include "renut_engine/gpu/overlay/app_integration.h"
#include "renut_engine/gpu/banjo_renut_capture.h"
#include "sa2_capture.h"
#endif

#if defined(MGR_NATIVE_RENDERER)
#include "cpu/sa2_enhancements.h"
#include "me/me_native_system.h"
#include "sa2_capture.h"
#include "me/me_startup_prewarm.h"
#include <imgui.h>

REXCVAR_DECLARE(bool, mgr_native_reuse_scratch);
REXCVAR_DECLARE(bool, mgr_wwise_native_dsp);
REXCVAR_DEFINE_BOOL(mgr_startup_prewarm, true, "MGR cache",
                    "Show graphics preparation progress and finish prewarming before starting the game")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {
class MgrPrewarmDialog final : public rex::ui::ImGuiDialog {
 public:
  explicit MgrPrewarmDialog(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}
  void OnDraw(ImGuiIO& io) override {
    const auto progress = me::native::startup::Read();
    const double now = ImGui::GetTime();
    if (started_ < 0) started_ = now;
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.025f, 0.045f, 0.065f, 1));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::Begin("##mgr-prewarm", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs);
    const float width = io.DisplaySize.x * 0.76f;
    ImGui::SetCursorPos(ImVec2(io.DisplaySize.x * 0.12f, io.DisplaySize.y * 0.34f));
    ImGui::BeginGroup();
    ImGui::TextColored(ImVec4(0.2f, 0.85f, 0.95f, 1), "METAL GEAR RISING: REVENGEANCE");
    ImGui::Spacing();
    ImGui::TextUnformatted("PREPARING GRAPHICS");
    ImGui::Spacing();
    if (progress.total) {
      ImGui::Text("Pipelines: %u / %u", progress.visited, progress.total);
      ImGui::ProgressBar(float(progress.visited) / float(progress.total), ImVec2(width, 22));
    } else {
      ImGui::TextUnformatted(progress.finished ? "No pipelines to prepare." : "Loading graphics cache...");
    }
    ImGui::Spacing();
    ImGui::TextUnformatted("The first launch may take longer. Please wait.");
    ImGui::Text("Elapsed: %.0f s", now - started_);
    if (progress.finished) {
      if (progress.failed || progress.missing) {
        ImGui::TextUnformatted("Some entries could not be prepared. Continuing to game...");
      } else {
        ImGui::TextUnformatted("Ready. Starting game...");
      }
    }
    ImGui::EndGroup();
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
  }
 private:
  double started_ = -1;
};
}  // namespace
#endif

#if defined(MGR_RENUT_RENDERER) && !defined(MGR_NATIVE_RENDERER) && REX_PLATFORM_SWITCH
// mgr-nx-deko (2026-10-22): the startup preparation of the deko3d build. Before the guest starts, every game shader is
// loaded and those the DKSH cache next to the NRO lacks are compiled (renut::gpu::PrewarmShaders), behind this screen,
// so no draw compiles a shader while playing.
#include <atomic>
#include <imgui.h>
#include <rex/ui/imgui_dialog.h>

REXCVAR_DEFINE_BOOL(mgr_startup_prewarm, true, "MGR cache",
                    "Show the shader preparation screen and load or compile every game shader before the game starts")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {
struct ShaderPrewarmState {
  std::atomic<uint32_t> done{0}, total{0}, compiled{0}, failed{0};
  std::atomic<bool> finished{false};
};
ShaderPrewarmState g_shader_prewarm;

class MgrShaderPrewarmDialog final : public rex::ui::ImGuiDialog {
 public:
  explicit MgrShaderPrewarmDialog(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}
  void OnDraw(ImGuiIO& io) override {
    const uint32_t done = g_shader_prewarm.done.load(), total = g_shader_prewarm.total.load();
    const uint32_t compiled = g_shader_prewarm.compiled.load(), failed = g_shader_prewarm.failed.load();
    const bool finished = g_shader_prewarm.finished.load();
    const double now = ImGui::GetTime();
    if (started_ < 0) started_ = now;
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.025f, 0.045f, 0.065f, 1));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::Begin("##mgr-shader-prewarm", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs);
    const float width = io.DisplaySize.x * 0.76f;
    ImGui::SetCursorPos(ImVec2(io.DisplaySize.x * 0.12f, io.DisplaySize.y * 0.34f));
    ImGui::BeginGroup();
    ImGui::TextColored(ImVec4(0.2f, 0.85f, 0.95f, 1), "METAL GEAR RISING: REVENGEANCE");
    ImGui::Spacing();
    ImGui::TextUnformatted("PREPARING SHADERS");
    ImGui::Spacing();
    if (total) {
      ImGui::Text("Shaders: %u / %u   (compiled now: %u)", done, total, compiled);
      ImGui::ProgressBar(float(done) / float(total), ImVec2(width, 22));
    } else {
      ImGui::TextUnformatted("Loading shader cache...");
    }
    ImGui::Spacing();
    ImGui::TextUnformatted("The first launch may take longer. Please wait.");
    ImGui::Text("Elapsed: %.0f s", now - started_);
    if (finished)
      ImGui::TextUnformatted(failed ? "Some shaders could not be prepared. Starting game..." : "Ready. Starting game...");
    ImGui::EndGroup();
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
  }

 private:
  double started_ = -1;
};
}  // namespace
#endif

class MgrApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<MgrApp>(new MgrApp(ctx, "mgr", PPCImageConfig));
  }

  std::string_view BuiltInConfig() const override {
#if defined(MGR_RENUT_RENDERER)
    return "sa2_renderer = \"renut\"\n"
#if defined(MGR_NATIVE_RENDERER)
           "sa2_renderer_native = false\n"
#endif
#if REX_PLATFORM_SWITCH
           "renut_gpu_backend = \"deko3d\"\n"
#else
           "renut_gpu_backend = \"vulkan\"\n"
#endif
           "input_xbox_layout = true\nmgr_shared_input = true\n"
           "sa2_renut_msaa = 0\nswitch_profile_report = false\nswitch_profile_stacks = false\n"
           "sa2_render_height_handheld = 360\nsa2_render_height_docked = 360\n";
#elif REX_PLATFORM_SWITCH
    // Performance baseline; explicit mgr.toml values still override these.
    return "sa2_renderer_native = true\ninput_xbox_layout = true\n"
           "sa2_render_height_handheld = 360\nsa2_render_height_docked = 360\n";
#else
    return {};
#endif
  }

  // The runtime defaults to no GPU plugin: select ReXGlue's Xenos implementation (emulated GPU, the reference of
  // porting-guide.md section 4), keeping an explicit command-line or config-file choice.
  void OnPostInitLogging() override {
#if defined(MGR_NATIVE_RENDERER) || defined(MGR_RENUT_RENDERER)
    sa2::enh::Resolve();
#endif
#if defined(MGR_RENUT_RENDERER)
    REXLOG_INFO("[mgr-deko] MGR reNut D3D renderer; upstream SA2 adf3a4eee524; built {} {}",
                __DATE__, __TIME__);
#endif
#if !REX_PLATFORM_SWITCH
    if (!rex::cvar::HasNonDefaultValue("gpu_plugin")) {
      rex::cvar::SetFlagByName("gpu_plugin", "xenos");
    }
#endif
#if defined(MGR_NATIVE_RENDERER)
    // Which of the renderer's switchable enhancements run (own key + group master), before any guest thread exists.
    REXLOG_INFO("[mgr-full] full-codegen-20261016 built {} {} scratch={} wwise_dsp={}",
                __DATE__, __TIME__, REXCVAR_GET(mgr_native_reuse_scratch),
                REXCVAR_GET(mgr_wwise_native_dsp));
    REXLOG_INFO("[mgr-fix] branch-renderer-20261016: verified dispatcher repair, restart16 and constant history");
    REXLOG_INFO("[mgr-direct] hotpaths-mgr-20261017: wide indices, fused vertex repack, bounded EDRAM scratch; direct-mgr-20261016 retained");
    REXLOG_INFO("[mgr-shadow] shadows-mgr-20261017: guarded MGR shadow geometry toggle; default off");
    REXLOG_INFO("[mgr-full] render heights handheld={} docked={} (explicit mgr.toml values override defaults)",
                rex::cvar::Query<int32_t>("sa2_render_height_handheld"),
                rex::cvar::Query<int32_t>("sa2_render_height_docked"));
#endif
  }

#if defined(MGR_NATIVE_RENDERER)
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
#if defined(MGR_RENUT_RENDERER)
    if (banjo::renut_gpu::Active()) return;
#endif
    if (me::native::Enabled() && REXCVAR_GET(mgr_startup_prewarm) &&
        rex::cvar::Query<bool>("sa2_native_pipelines_prewarm")) {
      prewarm_dialog_ = std::make_unique<MgrPrewarmDialog>(drawer);
    }
  }

  void LaunchModule() override {
    if (prewarm_dialog_) {
      REXLOG_INFO("[mgr-prewarm] Preparing graphics before guest launch");
      me::native::startup::Request();
      window()->RequestPresenterUIPaintFromUIThread();
      // Completion must not depend on drawing: a minimized window does not
      // paint, but still needs to launch the game after preparation finishes.
      prewarm_monitor_ = std::thread([this] {
        while (!prewarm_stop_.load(std::memory_order_acquire)) {
          const auto progress = me::native::startup::Read();
          if (!progress.finished) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
          }
          const int ticks = (progress.failed || progress.missing) ? 60 : 10;
          for (int i = 0; i < ticks; ++i) {
            if (prewarm_stop_.load(std::memory_order_acquire)) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
          }
          app_context().CallInUIThreadDeferred([this, progress] {
            if (closing_) return;
            REXLOG_INFO("[mgr-prewarm] Startup ready: {}/{} visited, {} prepared, {} missing, {} other settings, {} failed; launching game",
                        progress.visited, progress.total, progress.prepared, progress.missing,
                        progress.other, progress.failed);
            prewarm_dialog_.reset();
            ReXApp::LaunchModule();
          });
          return;
        }
      });
      return;
    }
    ReXApp::LaunchModule();
  }

  bool OnWindowCloseRequested() override {
    closing_ = true;
    prewarm_stop_.store(true, std::memory_order_release);
    return true;
  }
#endif

  // Stage 4: with sa2_renderer_native = true the graphics system is the app's native Vulkan renderer (src/me), and the
  // emulated Xenos GPU plugin is not loaded (ReXApp only loads it when config.graphics is empty).
  void OnPreSetup(rex::RuntimeConfig& config) override {
    // MGR is single-player and only guest user 0 has a signed-in profile.
    // With slot assignment an attached Joy-Con reserves user 0, and Start on
    // a Bluetooth pad selects a user that cannot finish the sign-in flow.
    config.input_factory = [](bool tool_mode) -> std::unique_ptr<rex::system::IInputSystem> {
      auto input = rex::input::CreateDefaultInputSystem(tool_mode);
      if (REXCVAR_GET(mgr_shared_input)) {
        input->SetDeviceAssignment(std::make_unique<rex::input::SharedAssignment>());
        REXLOG_INFO("[mgr-input] Shared player 1: all controllers use the signed-in profile");
      }
      return input;
    };
#if defined(MGR_RENUT_RENDERER)
    if (banjo::renut_gpu::Active()) {
      renut::gpu::ConfigureRuntime(config);
      REXLOG_INFO("graphics: MGR Direct3D through reNut/plume (no PM4 ring or EDRAM model)");
      return;
    }
#endif
#if defined(MGR_NATIVE_RENDERER)
    if (me::native::Enabled()) {
      config.graphics = me::native::CreateGraphicsSystem();
      REXLOG_INFO("graphics: native Vulkan renderer (PM4 ring)");
    }
#endif
  }

#if defined(MGR_RENUT_RENDERER)
#if defined(MGR_RENUT_RENDERER) && !defined(MGR_NATIVE_RENDERER) && REX_PLATFORM_SWITCH
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    if (banjo::renut_gpu::Active() && REXCVAR_GET(mgr_startup_prewarm))
      prewarm_dialog_ = std::make_unique<MgrShaderPrewarmDialog>(drawer);
  }

  // The guest starts once every shader is prepared; the preparation runs on its own thread (the UI thread keeps
  // drawing the screen), and its end does not depend on drawing.
  void LaunchModule() override {
    if (!prewarm_dialog_) {
      ReXApp::LaunchModule();
      return;
    }
    REXLOG_INFO("[mgr-prewarm] Preparing shaders before guest launch");
    window()->RequestPresenterUIPaintFromUIThread();
    prewarm_worker_ = std::thread([this] {
      renut::gpu::PrewarmShaders([](uint32_t done, uint32_t total, uint32_t compiled, uint32_t failed) {
        g_shader_prewarm.total.store(total);
        g_shader_prewarm.compiled.store(compiled);
        g_shader_prewarm.failed.store(failed);
        g_shader_prewarm.done.store(done);
      });
      g_shader_prewarm.finished.store(true);
      for (int i = 0; i < 10; ++i) {
        if (prewarm_stop_.load(std::memory_order_acquire)) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      app_context().CallInUIThreadDeferred([this] {
        if (closing_) return;
        REXLOG_INFO("[mgr-prewarm] Shaders ready: {} of {}, {} compiled now, {} failed; launching game",
                    g_shader_prewarm.done.load(), g_shader_prewarm.total.load(), g_shader_prewarm.compiled.load(),
                    g_shader_prewarm.failed.load());
        prewarm_dialog_.reset();
        ReXApp::LaunchModule();
      });
    });
  }

  bool OnWindowCloseRequested() override {
    closing_ = true;
    prewarm_stop_.store(true, std::memory_order_release);
    return true;
  }
#endif

  void OnPostLaunchModule(rex::system::XThread* thread) override {
    if (thread && banjo::renut_gpu::Active()) {
      // MGR indexes its five-way job scheduler by the guest PCR's logical CPU.
      // The SDK's default round-robin choice also counts host service threads;
      // removing the emulated GPU changes that choice and can select an
      // unregistered slot (-1). Set the primary guest CPU before it starts.
      // This changes guest-visible identity, independently of host core pinning.
      thread->SetAffinity(1u);
      REXLOG_INFO("[mgr-deko] Main guest thread starts on logical CPU 0");
    }
  }

  std::unique_ptr<rex::ui::ImmediateDrawer> OnCreateImmediateDrawer() override {
    if (banjo::renut_gpu::Active()) return renut::gpu::CreateOverlayDrawer();
    return ReXApp::OnCreateImmediateDrawer();
  }

  void OnWindowPixelSizeChanged(uint32_t width, uint32_t height) override {
    ReXApp::OnWindowPixelSizeChanged(width, height);
    if (banjo::renut_gpu::Active()) renut::gpu::OnWindowResized();
  }

  std::optional<rex::PathConfig> OnFinalizePaths(const rex::PathConfig& defaults,
      std::function<void(rex::PathConfig)> resume) override {
    if (banjo::renut_gpu::Active() &&
        !renut::gpu::StartRenderer(app_context(), [this] { return imgui_drawer(); })) {
      throw std::runtime_error("MGR: failed to start the reNut graphics backend");
    }
    return ReXApp::OnFinalizePaths(defaults, std::move(resume));
  }
#endif

  // Go/no-go measurements (MGR-PORT-PLAN.md, stage 3): --perf_log_csv=<file> writes one CSV row per presented frame
  // (draw_calls, fps, frame_time_us, vertices_processed, ...), counted by the emulated GPU. The SDK defines the cvar
  // and the writer (core/perf/counter.cpp) but nothing opens the file, so the app does. Only in builds with
  // REXGLUE_ENABLE_PERF_COUNTERS (every configuration but Release).
  void OnPostSetup() override {
#if defined(MGR_NATIVE_RENDERER) || defined(MGR_RENUT_RENDERER)
    sa2::enh::StartReport();
    sa2::render::RegisterKeys();
#endif
#if defined(MGR_RENUT_RENDERER)
    if (banjo::renut_gpu::Active()) {
      sa2::capture::StartWithSource([](rex::ui::RawImage& image) {
        return renut::gpu::CaptureFrame(image);
      });
    }
#endif
#if defined(MGR_NATIVE_RENDERER)
    #if !REX_PLATFORM_SWITCH
    rex::ui::RegisterBind("bind_mgr_fps_test", "F10", "Toggle the MGR 30 FPS speed test", [] {
      const int next = rex::cvar::Query<int32_t>("mgr_fps_limit") == 30 ? 0 : 30;
      rex::cvar::SetFlagByName("mgr_fps_limit", std::to_string(next));
      REXLOG_INFO("[mgr-bench] FPS cap changed to {} (0 = game default; guest clock unchanged)", next);
    });
    #endif
#if defined(MGR_RENUT_RENDERER)
    if (!banjo::renut_gpu::Active())
#endif
    sa2::capture::Start([this]() -> rex::ui::Presenter* {
      const auto* rt = runtime();
      const auto* graphics = rt ? rt->graphics_system() : nullptr;
      return graphics ? graphics->presenter() : nullptr;
    });
#endif
    const auto path = rex::cvar::Query<std::string>("perf_log_csv");
    if (path.empty()) {
      return;
    }
#if defined(REXGLUE_ENABLE_PERF_COUNTERS)
    rex::perf::SetCsvLogPath(path);
    REXLOG_INFO("perf: per-frame counters written to {}", path);
#else
    REXLOG_WARN("perf: perf_log_csv ignored, this build has no perf counters (Release)");
#endif
  }

  void OnShutdown() override {
#if defined(MGR_RENUT_RENDERER)
    sa2::capture::Stop();
    if (banjo::renut_gpu::Active()) renut::gpu::ShutdownRenderer();
#endif
#if defined(MGR_RENUT_RENDERER) && !defined(MGR_NATIVE_RENDERER) && REX_PLATFORM_SWITCH
    closing_ = true;
    prewarm_stop_.store(true, std::memory_order_release);
    if (prewarm_worker_.joinable()) prewarm_worker_.join();
    prewarm_dialog_.reset();
#endif
#if defined(MGR_NATIVE_RENDERER)
    closing_ = true;
    prewarm_stop_.store(true, std::memory_order_release);
    if (prewarm_monitor_.joinable()) prewarm_monitor_.join();
    prewarm_dialog_.reset();
    sa2::capture::Stop();
#endif
  }

  // Without --game_data_root, look for game_root/ or assets/game_root/ next to the executable or in a parent folder.
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
        if (std::filesystem::is_regular_file(candidate / sub / "default.xex", ec)) {
          paths.game_data_root = candidate / sub;
          return;
        }
      }
      if (candidate == candidate.parent_path()) {
        break;
      }
    }
  }

#if defined(MGR_RENUT_RENDERER) && !defined(MGR_NATIVE_RENDERER) && REX_PLATFORM_SWITCH
 private:
  std::unique_ptr<MgrShaderPrewarmDialog> prewarm_dialog_;
  std::thread prewarm_worker_;
  std::atomic<bool> prewarm_stop_{false};
  bool closing_ = false;
#endif
#if defined(MGR_NATIVE_RENDERER)
 private:
  std::unique_ptr<MgrPrewarmDialog> prewarm_dialog_;
  std::thread prewarm_monitor_;
  std::atomic<bool> prewarm_stop_{false};
  bool closing_ = false;
#endif
};

REX_DEFINE_APP(mgr, MgrApp::Create)
