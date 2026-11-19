#include "toolchain.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>

#include <SDL3/SDL.h>

namespace toolchain {

namespace {

[[maybe_unused]] std::string Env(const char* name) {
  const char* v = SDL_getenv(name);
  return v ? std::string(v) : std::string();
}

std::string FirstLine(const std::string& text) {
  auto lines = util::SplitLines(text);
  return lines.empty() ? std::string() : util::Trim(lines[0]);
}

// Finds "major.minor[.patch]" after `marker` in text.
bool ParseVersion(const std::string& text, const char* marker, int& major, int& minor, std::string& full) {
  size_t at = text.find(marker);
  if (at == std::string::npos) return false;
  at += std::char_traits<char>::length(marker);
  size_t end = at;
  while (end < text.size() && (std::isdigit(static_cast<unsigned char>(text[end])) || text[end] == '.')) ++end;
  full = text.substr(at, end - at);
  if (std::sscanf(full.c_str(), "%d.%d", &major, &minor) < 1) return false;
  return true;
}

std::string VersionOf(const std::string& exe, const char* marker, int* major_out = nullptr, int* minor_out = nullptr) {
  proc::Command c;
  c.args = {exe, "--version"};
  std::string out;
  if (proc::Capture(c, &out, nullptr) != 0) return {};
  int major = 0, minor = 0;
  std::string full;
  if (!ParseVersion(out, marker, major, minor, full)) return FirstLine(out);
  if (major_out) *major_out = major;
  if (minor_out) *minor_out = minor;
  return full;
}

void AddPathFront(proc::Env& env, const std::string& dir) {
  if (dir.empty()) return;
#ifdef _WIN32
  const char sep = ';';
#else
  const char sep = ':';
#endif
  std::string& path = env["PATH"];
  path = path.empty() ? dir : dir + sep + path;
}

std::string ParentDir(const std::string& file) { return util::PathStr(util::U8Path(file).parent_path()); }

}  // namespace

const char* PresetName() {
#ifdef _WIN32
  return "win-amd64-release";
#else
  return "linux-amd64-release";
#endif
}

const char* GameExeFileName() {
#ifdef _WIN32
  return "gundam_operation_troy.exe";
#else
  return "gundam_operation_troy";
#endif
}

std::string InstallerComponents() {
  return "--add Microsoft.VisualStudio.Workload.VCTools --includeRecommended "
         "--add Microsoft.VisualStudio.Component.VC.Llvm.Clang "
         "--add Microsoft.VisualStudio.Component.VC.Llvm.ClangToolset "
         "--add Microsoft.VisualStudio.Component.VC.CMake.Project";
}

std::string Info::Summary() const {
  if (!missing.empty()) {
    std::string s = "Missing: ";
    for (size_t i = 0; i < missing.size(); ++i) s += (i ? ", " : "") + missing[i];
    return s;
  }
  std::string s;
  if (!vs_name.empty()) s += vs_name + "  /  ";
  s += "clang " + clang_version + "  /  CMake " + cmake_version;
  if (!ninja_version.empty()) s += "  /  Ninja " + ninja_version;
  return s;
}

Info Detect(const fs::path& tools_dir) {
  Info info;
#ifdef _WIN32
  const fs::path pf = util::U8Path(Env("ProgramFiles"));
  const fs::path pf86 = util::U8Path(Env("ProgramFiles(x86)"));
  const fs::path sys32 = util::U8Path(Env("SystemRoot")) / "System32";

  // Visual Studio (or its Build Tools) with the C++ tools.
  const fs::path vswhere = pf86 / "Microsoft Visual Studio" / "Installer" / "vswhere.exe";
  if (util::IsFile(vswhere)) {
    auto query = [&](const char* property) {
      proc::Command c;
      c.args = {util::PathStr(vswhere), "-latest", "-products", "*", "-requires",
                "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", property, "-utf8"};
      std::string out;
      return proc::Capture(c, &out, nullptr) == 0 ? FirstLine(out) : std::string();
    };
    info.vs_path = query("installationPath");
    if (!info.vs_path.empty()) {
      info.vs_name = query("displayName");
      info.vs_year = std::atoi(query("catalog_productLineVersion").c_str());
    }
  }
  const fs::path vs = util::U8Path(info.vs_path);
  if (info.vs_path.empty()) {
    info.missing.push_back("Visual Studio Build Tools (C++)");
  } else {
    const fs::path vcvars = vs / "VC" / "Auxiliary" / "Build" / "vcvars64.bat";
    if (util::IsFile(vcvars))
      info.vcvars = util::PathStr(vcvars);
    else
      info.missing.push_back("Visual Studio C++ tools (vcvars64.bat)");
  }

  // clang 18 or newer: Visual Studio's own copy first, then a standalone LLVM.
  std::vector<fs::path> clang_dirs;
  if (!info.vs_path.empty()) clang_dirs.push_back(vs / "VC" / "Tools" / "Llvm" / "x64" / "bin");
  clang_dirs.push_back(pf / "LLVM" / "bin");
  for (const auto& dir : clang_dirs) {
    const fs::path exe = dir / "clang++.exe";
    if (!util::IsFile(exe)) continue;
    int major = 0;
    std::string version = VersionOf(util::PathStr(exe), "version ", &major);
    if (major >= 18) {
      info.clang_dir = util::PathStr(dir);
      info.clang_version = version;
      info.clang_major = major;
      break;
    }
    if (info.old_clang.empty()) info.old_clang = version;
  }
  if (info.clang_dir.empty())
    info.missing.push_back(info.old_clang.empty() ? "clang 18 or newer"
                                                  : "clang 18 or newer (found " + info.old_clang + ")");

  // CMake 3.25 or newer.
  std::vector<std::string> cmakes;
  if (!info.vs_path.empty())
    cmakes.push_back(util::PathStr(vs / "Common7" / "IDE" / "CommonExtensions" / "Microsoft" / "CMake" / "CMake" / "bin" / "cmake.exe"));
  cmakes.push_back(util::PathStr(pf / "CMake" / "bin" / "cmake.exe"));
  cmakes.push_back(proc::FindOnPath("cmake", nullptr));
  for (const auto& exe : cmakes) {
    if (exe.empty() || !util::IsFile(util::U8Path(exe))) continue;
    int major = 0, minor = 0;
    std::string version = VersionOf(exe, "cmake version ", &major, &minor);
    if (major > 3 || (major == 3 && minor >= 25)) {
      info.cmake = exe;
      info.cmake_version = version;
      break;
    }
  }
  if (info.cmake.empty()) info.missing.push_back("CMake 3.25 or newer");

  // Ninja: Visual Studio's, PATH, or the copy the launcher downloads itself.
  std::vector<std::string> ninjas;
  if (!info.vs_path.empty())
    ninjas.push_back(util::PathStr(vs / "Common7" / "IDE" / "CommonExtensions" / "Microsoft" / "CMake" / "Ninja" / "ninja.exe"));
  ninjas.push_back(proc::FindOnPath("ninja", nullptr));
  ninjas.push_back(util::PathStr(tools_dir / "ninja" / "ninja.exe"));
  for (const auto& exe : ninjas) {
    if (exe.empty() || !util::IsFile(util::U8Path(exe))) continue;
    info.ninja = exe;
    info.ninja_version = VersionOf(exe, "");
    break;
  }

  info.git = proc::FindOnPath("git", nullptr);
  if (info.git.empty() && util::IsFile(pf / "Git" / "cmd" / "git.exe")) info.git = util::PathStr(pf / "Git" / "cmd" / "git.exe");
  if (util::IsFile(sys32 / "curl.exe")) info.curl = util::PathStr(sys32 / "curl.exe");
  if (util::IsFile(sys32 / "tar.exe")) info.tar = util::PathStr(sys32 / "tar.exe");

  // Ninja alone can be fetched by the launcher; the rest needs Microsoft's installer.
  info.can_install = !info.missing.empty();
#else
  for (const char* name : {"clang++-20", "clang++-19", "clang++-18", "clang++"}) {
    std::string exe = proc::FindOnPath(name, nullptr);
    if (exe.empty()) continue;
    int major = 0;
    std::string version = VersionOf(exe, "version ", &major);
    if (major >= 18) {
      info.clang_dir = ParentDir(exe);
      info.clang_version = version;
      info.clang_major = major;
      break;
    }
  }
  if (info.clang_dir.empty()) info.missing.push_back("clang 18 or newer");
  info.cmake = proc::FindOnPath("cmake", nullptr);
  if (!info.cmake.empty()) {
    int major = 0, minor = 0;
    info.cmake_version = VersionOf(info.cmake, "cmake version ", &major, &minor);
    if (!(major > 3 || (major == 3 && minor >= 25))) info.cmake.clear();
  }
  if (info.cmake.empty()) info.missing.push_back("CMake 3.25 or newer");
  info.ninja = proc::FindOnPath("ninja", nullptr);
  if (!info.ninja.empty()) info.ninja_version = VersionOf(info.ninja, "");
  info.git = proc::FindOnPath("git", nullptr);
  info.curl = proc::FindOnPath("curl", nullptr);
  info.tar = proc::FindOnPath("tar", nullptr);
#endif
  if (info.ninja.empty() && info.curl.empty()) info.missing.push_back("Ninja");
  info.ok = info.missing.empty();
  return info;
}

bool BuildEnvironment(const Info& info, proc::Env& env, std::string& error) {
#ifdef _WIN32
  std::string comspec = Env("ComSpec");
  if (comspec.empty()) comspec = "cmd.exe";
  proc::Command c;
  c.raw_cmdline = proc::QuoteArg(comspec) + " /d /s /c \"\"" + info.vcvars + "\" >nul 2>&1 && set\"";
  std::string out;
  if (proc::Capture(c, &out, &error) != 0) {
    if (error.empty()) error = "Visual Studio's C++ environment (vcvars64.bat) could not be loaded.";
    return false;
  }
  env.clear();
  for (const std::string& line : util::SplitLines(out)) {
    size_t eq = line.find('=');
    if (eq == std::string::npos || eq == 0) continue;
    env[line.substr(0, eq)] = line.substr(eq + 1);
  }
  if (env.find("VCToolsInstallDir") == env.end()) {
    error = "Visual Studio's C++ environment (vcvars64.bat) could not be loaded.";
    return false;
  }
  if (env.find("WindowsSdkDir") == env.end()) {
    error = "The Windows SDK is missing. Add it in the Visual Studio Installer (Desktop development with C++).";
    return false;
  }
#else
  env = proc::CurrentEnvironment();
#endif
  if (!info.ninja.empty()) AddPathFront(env, ParentDir(info.ninja));
  if (!info.cmake.empty()) AddPathFront(env, ParentDir(info.cmake));
  AddPathFront(env, info.clang_dir);
  return true;
}

}  // namespace toolchain
