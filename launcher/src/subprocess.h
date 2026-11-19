// Starting other programs: the build tools (with their output read line by line)
// and the game itself.
#pragma once

#include <atomic>
#include <climits>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace proc {

struct CaseInsensitiveLess {
  bool operator()(const std::string& a, const std::string& b) const;
};

#ifdef _WIN32
using Env = std::map<std::string, std::string, CaseInsensitiveLess>;  // names ignore case
#else
using Env = std::map<std::string, std::string>;
#endif

Env CurrentEnvironment();

struct Command {
  std::vector<std::string> args;  // program, then its arguments
  std::string raw_cmdline;        // Windows only: given to CreateProcess unchanged (args ignored)
  std::string cwd;                // working folder (empty: the launcher's)
  const Env* env = nullptr;       // full environment (nullptr: the launcher's)
};

using LineFn = std::function<void(std::string_view)>;

constexpr int kStartFailed = INT_MIN;
constexpr int kCancelled = INT_MIN + 1;

// Runs a console program to completion; its output (stdout and stderr together) is
// passed to on_line one line at a time. Returns its exit code, kStartFailed (error
// is filled in), or kCancelled once *cancel became true (on Windows everything it
// started is stopped too). No console window appears.
int Run(const Command& cmd, const LineFn& on_line, const std::atomic<bool>* cancel, std::string* error);

// Runs a program and collects all of its output.
int Capture(const Command& cmd, std::string* output, std::string* error);

// A program started without waiting for it (the game).
class Child {
 public:
  Child() = default;
  Child(const Child&) = delete;
  Child& operator=(const Child&) = delete;
  ~Child();
  bool Start(const Command& cmd, std::string* error);
  bool Running();  // polls; false once it has exited
  int ExitCode() const { return exit_code_; }

 private:
  void* handle_ = nullptr;
  bool running_ = false;
  int exit_code_ = 0;
};

// Opens a folder, file or web address with the system (Explorer, browser).
bool OpenWithSystem(const std::string& path_or_url);

// Runs a program that may ask for administrator rights (Windows UAC), waits for it,
// and returns its exit code (kStartFailed if it could not start or was declined).
int RunShell(const std::string& exe, const std::string& params, const std::atomic<bool>* cancel,
             std::string* error);

// Looks for a program on PATH (Windows adds .exe). Returns its full path or "".
std::string FindOnPath(const std::string& name, const Env* env);

// Quotes one argument for a Windows command line (MSVC rules).
std::string QuoteArg(const std::string& arg);
std::string JoinArgs(const std::vector<std::string>& args);

}  // namespace proc
