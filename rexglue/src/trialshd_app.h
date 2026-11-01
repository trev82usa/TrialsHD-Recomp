// trialshd - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include "crash_handler.h"
#include "perf_tools.h"
#include <rex/rex_app.h>

#include <cstdlib>
#include <filesystem>

class TrialshdApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<TrialshdApp>(new TrialshdApp(ctx, "trialshd",
        PPCImageConfig));
  }

  // Override virtual hooks for customization:
  // void OnPostInitLogging() override {}
  // Use the Xenos GPU backend unless --gpu_plugin says otherwise. Setting TRIALSHD_NO_GPU=1
  // runs headless (used for testing on machines without a GPU).
  void OnPreSetup(rex::RuntimeConfig& config) override {
    SetHighTimerResolution();
    if (config.gpu_plugin.empty() && !std::getenv("TRIALSHD_NO_GPU")) config.gpu_plugin = "xenos";
    // Full game, not the Xbox Live Arcade trial: report every license bit as granted to
    // XamContentGetLicenseMask (ReXGlue defaults to 0 = trial).
    rex::cvar::SetFlagByName("license_mask", "4294967295");
  }
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostLoadXexImage() override {}
  // Installed after setup so nothing in the runtime replaces it.
  void OnPostSetup() override {
    InstallCrashHandler();
    SetGuestFrameStats([] { return GetGuestFrameStats(); });
    StartProfilerIfEnabled();
  }
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // std::unique_ptr<rex::ui::AchievementNotificationDialog>
  // CreateAchievementNotificationDialog() override;
  // void OnShutdown() override {}

  // Find the game files without a command-line argument: an "assets" folder next to the
  // exe, or default.xex placed right next to the exe.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    if (!paths.game_data_root.empty()) return;   // --game_data_root given: respect it
    auto exe_dir = rex::filesystem::GetExecutableFolder();
    for (const auto& dir : {exe_dir / "assets", exe_dir}) {
      std::error_code ec;
      if (std::filesystem::exists(dir / "default.xex", ec)) {
        paths.game_data_root = dir;
        return;
      }
    }
  }
};
