// Finding the build tools the recomp needs on the player's PC.
#pragma once

#include <string>
#include <vector>

#include "subprocess.h"
#include "util.h"

namespace toolchain {

struct Info {
  bool ok = false;  // everything needed to build is present
  std::string vs_path, vs_name, vcvars;  // Visual Studio (Windows)
  int vs_year = 0;                       // 2019, 2022, ... (0 if unknown)
  std::string old_clang;                 // version of a clang found that is older than 18
  std::string clang_dir, clang_version;
  int clang_major = 0;
  std::string cmake, cmake_version;
  std::string ninja, ninja_version;
  std::string git;
  std::string curl, tar;
  std::vector<std::string> missing;  // plain names of what is missing
  bool can_install = false;          // the Build Tools installer can fix what is missing
  std::string Summary() const;
};

// tools_dir: where the launcher keeps tools it downloaded itself (Ninja).
Info Detect(const fs::path& tools_dir);

// The environment the compiler runs in: Visual Studio's (vcvars64.bat) on Windows,
// with clang, CMake and Ninja first on PATH.
bool BuildEnvironment(const Info& info, proc::Env& env, std::string& error);

const char* PresetName();      // CMake preset used for the game
const char* GameExeFileName();  // gundam_operation_troy(.exe)

// Visual Studio Build Tools components the build needs (for the installer).
std::string InstallerComponents();

}  // namespace toolchain
