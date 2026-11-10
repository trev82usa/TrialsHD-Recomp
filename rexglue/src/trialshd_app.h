// trialshd - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include "crash_handler.h"
#include "perf_tools.h"
#include "trials_menu.h"
#include "keyboard_input.h"
#include <rex/rex_app.h>
#include <rex/ui/keybinds.h>
#include <rex/input/input_system.h>
#include <rex/runtime.h>

#include <chrono>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <rex/logging/api.h>
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
    if (rex::cvar::Query<bool>("trialshd_crash_handler")) InstallCrashHandler();
    SetGuestFrameStats([] { return GetGuestFrameStats(); });
    StartProfilerIfEnabled();
    // After the SDK set its own input rule (in ConstructRuntime), so ours replaces it.
    if (runtime() && runtime()->input_system()) {
      auto* input = static_cast<rex::input::InputSystem*>(runtime()->input_system());
      auto keyboard = CreateKeyboardDriver(window());
      auto* keyboard_ptr = keyboard.get();
      input->AddDriver(std::move(keyboard));  // before the callback is set
      MoveKeyboardDriverFirst(input, keyboard_ptr);
      ConnectTrialsMenuInput(input);
    }
  }
  // F1: the Trials HD settings menu (name, password, online, graphics, sound).
  // Controller: LB+RB+X.
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    InitTrialsMenu(drawer, window(),
                   rex::filesystem::GetExecutableFolder() / "trialshd.toml");
    rex::ui::RegisterBind("bind_trials_menu", "F1", "Toggle the Trials HD settings menu",
                          [] { ToggleTrialsMenu(); });
  }
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // std::unique_ptr<rex::ui::AchievementNotificationDialog>
  // CreateAchievementNotificationDialog() override;
  // Closing the window runs the engine's full teardown, which can hang on a game thread that
  // never stops (the process then lingers invisibly). Give it a few seconds, then save the log
  // and end the process; scores are already uploaded by then.
  bool OnWindowCloseRequested() override {
    StartExitWatchdog();
    return true;
  }
  void OnShutdown() override { StartExitWatchdog(); }

  static void StartExitWatchdog() {
    static std::once_flag once;
    std::call_once(once, [] {
      std::thread([] {
        std::this_thread::sleep_for(std::chrono::seconds(3));
        rex::FlushLogging();
        std::_Exit(0);
      }).detach();
    });
  }

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
