// pgr3 - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/rex_app.h>

#ifdef _WIN32
#include <timeapi.h>
#endif

#include "icon.generated.h"

#include "platform/guest_abort_log.h"
#include "platform/silent_audio.h"
#include <rex/cvar.h>

REXCVAR_DECLARE(bool, pgr3_null_audio);

class Pgr3App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Pgr3App>(new Pgr3App(ctx, "pgr3",
        PPCImageConfig));
  }

  void OnPreSetup(rex::RuntimeConfig& config) override {
	  config.gpu_plugin = "xenos";
      if (REXCVAR_GET(pgr3_null_audio))
        config.audio_factory = REX_AUDIO_BACKEND(pgr3::platform::SilentAudioSystem);
#ifdef _WIN32
      timeBeginPeriod(1);
#endif
  }

  void OnConfigurePaths(rex::PathConfig& paths) override {
      const auto base_dir = paths.config_path.parent_path();
      if (paths.game_data_root.empty()) { // Use default assets directory path if one isn't provided!
          if (std::filesystem::is_regular_file(base_dir / "assets/default.xex")) {
              paths.game_data_root = base_dir / "assets";
          }
      }
      paths.user_data_root = base_dir / "user";
      paths.cache_root = base_dir / "cache";
  }

  void OnPostSetup() override{
    window()->SetIcon(icon::kIconPNG, icon::kIconPNGSize);
    window()->SetTitle("Project Gotham Racing 3");
  }

  void OnPreLaunchModule() override {
    pgr3::platform::InstallGuestAbortLogger();
  }

  // Override virtual hooks for customization:
  // void OnPostInitLogging() override {}
  // void OnPreSetup(rex::RuntimeConfig& config) override {}
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostLoadXexImage() override {}
  // void OnPostSetup() override {}
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // std::unique_ptr<rex::ui::AchievementNotificationDialog>
  // CreateAchievementNotificationDialog() override;
  // void OnShutdown() override {}
  // void OnConfigurePaths(rex::PathConfig& paths) override {}
};
