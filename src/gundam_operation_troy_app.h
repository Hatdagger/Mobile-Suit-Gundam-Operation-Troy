// gundam_operation_troy - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <filesystem>
#include <system_error>

#include <rex/filesystem.h>
#include <rex/rex_app.h>

class GundamOperationTroyApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<GundamOperationTroyApp>(new GundamOperationTroyApp(ctx, "gundam_operation_troy",
        PPCImageConfig));
  }

  // If no game folder was given (--game_data_root or the .toml config), look for
  // the extracted game in a "game" folder next to the executable, then in the
  // folders above it. This finds the repository's game/ folder when running a
  // development build from out/build/<preset>/.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    if (!paths.game_data_root.empty()) {
      return;
    }
    std::error_code ec;
    std::filesystem::path dir = rex::filesystem::GetExecutableFolder();
    for (int depth = 0; depth < 5 && !dir.empty(); ++depth) {
      const auto candidate = dir / "game";
      if (std::filesystem::exists(candidate / "default.xex", ec)) {
        paths.game_data_root = candidate;
        return;
      }
      const auto parent = dir.parent_path();
      if (parent == dir) {
        break;
      }
      dir = parent;
    }
  }

  // Graphics: load the SDK's Xenos GPU emulation plugin (rexgpu-xenos.dll next to
  // the exe). Without it the game runs with no graphics at all (black window).
  // A gpu_plugin line in gundam_operation_troy.toml still takes precedence.
  void OnPreSetup(rex::RuntimeConfig& config) override {
    if (config.gpu_plugin.empty()) {
      config.gpu_plugin = "xenos";
    }
  }

  // Other hooks available for customization:
  // void OnPostInitLogging() override {}
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostLoadXexImage() override {}
  // void OnPostSetup() override {}
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
  // void OnShutdown() override {}
};
