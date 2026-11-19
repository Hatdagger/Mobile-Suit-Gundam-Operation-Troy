// Operation Troy launcher: window, renderer and main loop.
//
//   "Operation Troy.exe"                    normal use
//   "Operation Troy.exe" --root <folder>    act as if the launcher were in <folder>
//   "Operation Troy.exe" --screenshots <dir> [--size 1280x720]
//        renders every screen with example data to <dir>/*.bmp and exits (used to
//        check the look on machines without a display).

#include <cstdio>
#include <string>

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "app.h"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "ui.h"

namespace {

void RenderFrame(SDL_Renderer* renderer, App& app, float dt) {
  ImGui_ImplSDLRenderer3_NewFrame();
  ImGui_ImplSDL3_NewFrame();
  ImGui::NewFrame();
  app.Frame(dt);
  ImGui::Render();
  SDL_SetRenderDrawColor(renderer, 3, 6, 11, 255);
  SDL_RenderClear(renderer);
  ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
}

bool SaveScreenshot(SDL_Renderer* renderer, const std::string& path) {
  SDL_Surface* surface = SDL_RenderReadPixels(renderer, nullptr);
  if (!surface) return false;
  const bool ok = SDL_SaveBMP(surface, path.c_str());
  SDL_DestroySurface(surface);
  return ok;
}

}  // namespace

int main(int argc, char** argv) {
  LaunchOptions options;
  int width = 1280, height = 720;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--screenshots" && i + 1 < argc)
      options.screenshot_dir = argv[++i];
    else if (arg == "--root" && i + 1 < argc)
      options.root = argv[++i];
    else if (arg == "--size" && i + 1 < argc)
      std::sscanf(argv[++i], "%dx%d", &width, &height);
  }
  const bool screenshots = !options.screenshot_dir.empty();
  if (screenshots) SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
  SDL_SetAppMetadata("Operation Troy", OPTROY_LAUNCHER_VERSION, "io.github.hatdagger.operationtroy");

  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Operation Troy", SDL_GetError(), nullptr);
    return 1;
  }
  SDL_Window* window = SDL_CreateWindow("Operation Troy", width, height,
                                        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  if (!window) {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Operation Troy", SDL_GetError(), nullptr);
    SDL_Quit();
    return 1;
  }
  SDL_SetWindowMinimumSize(window, 640, 360);
  SDL_Renderer* renderer = SDL_CreateRenderer(window, screenshots ? "software" : nullptr);
  if (!renderer) {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Operation Troy", SDL_GetError(), window);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }
  SDL_SetRenderVSync(renderer, 1);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.LogFilename = nullptr;
  // Geometry-based line anti-aliasing: identical on every renderer (including the
  // software renderer used for screenshots).
  ImGui::GetStyle().AntiAliasedLinesUseTex = false;
  ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
  ImGui_ImplSDLRenderer3_Init(renderer);
  ui::LoadFonts();

  int exit_code = 0;
  {
    App app(options);
    if (!app.Init(window)) {
      exit_code = 1;
    } else if (screenshots) {
      for (int i = 0; i < App::DemoCount(); ++i) {
        app.LoadDemo(i);
        for (int frame = 0; frame < 4; ++frame) {
          SDL_Event e;
          while (SDL_PollEvent(&e)) ImGui_ImplSDL3_ProcessEvent(&e);
          RenderFrame(renderer, app, 1.f / 60.f);
        }
        const std::string path = options.screenshot_dir + "/" + App::DemoName(i) + ".bmp";
        if (!SaveScreenshot(renderer, path)) {
          std::fprintf(stderr, "could not save %s: %s\n", path.c_str(), SDL_GetError());
          exit_code = 1;
        }
        SDL_RenderPresent(renderer);
      }
    } else {
      Uint64 last = SDL_GetTicksNS();
      while (!app.WantsQuit()) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
          ImGui_ImplSDL3_ProcessEvent(&e);
          app.HandleEvent(e);
        }
        const Uint64 now = SDL_GetTicksNS();
        float dt = float(double(now - last) / 1e9);
        last = now;
        if (dt > 0.1f) dt = 0.1f;
        const SDL_WindowFlags flags = SDL_GetWindowFlags(window);
        if (flags & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_OCCLUDED)) {
          app.Tick();  // keep watching the game and the build
          SDL_Delay(50);
          continue;
        }
        RenderFrame(renderer, app, dt);
        SDL_RenderPresent(renderer);
        if (!(flags & SDL_WINDOW_INPUT_FOCUS)) SDL_Delay(25);  // stay light in the background
      }
    }
  }

  ImGui_ImplSDLRenderer3_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return exit_code;
}
