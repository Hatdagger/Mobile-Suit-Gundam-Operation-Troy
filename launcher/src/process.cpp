#include "process.h"

#include <algorithm>
#include <cstring>

#include <SDL3/SDL.h>

#include "util.h"

#ifdef _WIN32
#include <cwchar>
#include <windows.h>
#include <shellapi.h>
#else
#include <unistd.h>
extern char** environ;
#endif

namespace proc {

bool CaseInsensitiveLess::operator()(const std::string& a, const std::string& b) const {
  const size_t n = std::min(a.size(), b.size());
  for (size_t i = 0; i < n; ++i) {
    char ca = a[i], cb = b[i];
    if (ca >= 'a' && ca <= 'z') ca = char(ca - 'a' + 'A');
    if (cb >= 'a' && cb <= 'z') cb = char(cb - 'a' + 'A');
    if (ca != cb) return static_cast<unsigned char>(ca) < static_cast<unsigned char>(cb);
  }
  return a.size() < b.size();
}

namespace {

// Splits a byte stream into lines on \n, \r\n and lone \r (progress meters).
class LineSplitter {
 public:
  explicit LineSplitter(const LineFn& fn) : fn_(fn) {}
  void Feed(const char* data, size_t size) {
    for (size_t i = 0; i < size; ++i) {
      const char c = data[i];
      if (c == '\n' || c == '\r') {
        Emit();
      } else {
        pending_.push_back(c);
        if (pending_.size() > 16384) Emit();
      }
    }
  }
  void Flush() { Emit(); }

 private:
  void Emit() {
    if (!pending_.empty() && fn_) fn_(pending_);
    pending_.clear();
  }
  const LineFn& fn_;
  std::string pending_;
};

}  // namespace

std::string QuoteArg(const std::string& arg) {
  if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string::npos) return arg;
  std::string out = "\"";
  for (size_t i = 0;; ++i) {
    size_t backslashes = 0;
    while (i < arg.size() && arg[i] == '\\') {
      ++i;
      ++backslashes;
    }
    if (i == arg.size()) {
      out.append(backslashes * 2, '\\');
      break;
    }
    if (arg[i] == '"') {
      out.append(backslashes * 2 + 1, '\\');
      out.push_back('"');
    } else {
      out.append(backslashes, '\\');
      out.push_back(arg[i]);
    }
  }
  out.push_back('"');
  return out;
}

std::string JoinArgs(const std::vector<std::string>& args) {
  std::string out;
  for (const auto& a : args) {
    if (!out.empty()) out.push_back(' ');
    out += QuoteArg(a);
  }
  return out;
}

int Capture(const Command& cmd, std::string* output, std::string* error) {
  std::string collected;
  int code = Run(
      cmd,
      [&](std::string_view line) {
        collected.append(line);
        collected.push_back('\n');
      },
      nullptr, error);
  if (output) *output = std::move(collected);
  return code;
}

std::string FindOnPath(const std::string& name, const Env* env) {
  std::string path_value;
  if (env) {
    auto it = env->find("PATH");
    if (it != env->end()) path_value = it->second;
  } else if (const char* p = SDL_getenv("PATH")) {
    path_value = p;
  }
#ifdef _WIN32
  const char sep = ';';
  std::vector<std::string> names = {name};
  if (!util::EndsWithI(name, ".exe") && !util::EndsWithI(name, ".bat") && !util::EndsWithI(name, ".cmd"))
    names = {name + ".exe", name + ".cmd", name + ".bat"};
#else
  const char sep = ':';
  std::vector<std::string> names = {name};
#endif
  size_t start = 0;
  while (start <= path_value.size()) {
    size_t end = path_value.find(sep, start);
    if (end == std::string::npos) end = path_value.size();
    std::string dir = util::Trim(path_value.substr(start, end - start));
    if (!dir.empty() && dir.front() == '"' && dir.back() == '"' && dir.size() >= 2)
      dir = dir.substr(1, dir.size() - 2);
    if (!dir.empty()) {
      for (const auto& n : names) {
        fs::path candidate = util::U8Path(dir) / util::U8Path(n);
        if (util::IsFile(candidate)) return util::PathStr(candidate);
      }
    }
    start = end + 1;
  }
  return {};
}

#ifdef _WIN32
// ===================================================================================
// Windows
// ===================================================================================

namespace {

std::wstring Widen(std::string_view s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

std::string Narrow(std::wstring_view w) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}

std::string ErrorText(DWORD err) {
  wchar_t* buf = nullptr;
  FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                 nullptr, err, 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
  std::string text = buf ? util::Trim(Narrow(buf)) : std::string();
  if (buf) LocalFree(buf);
  if (text.empty()) text = "error " + std::to_string(err);
  return text;
}

std::wstring EnvBlock(const Env& env) {
  std::wstring block;
  for (const auto& [k, v] : env) {
    if (k.empty()) continue;
    block += Widen(k);
    block += L'=';
    block += Widen(v);
    block += L'\0';
  }
  block += L'\0';
  return block;
}

std::string ProgramName(const Command& cmd) {
  if (!cmd.args.empty()) return cmd.args[0];
  return cmd.raw_cmdline.substr(0, cmd.raw_cmdline.find(' '));
}

}  // namespace

Env CurrentEnvironment() {
  Env env;
  wchar_t* block = GetEnvironmentStringsW();
  if (!block) return env;
  for (const wchar_t* p = block; *p; p += wcslen(p) + 1) {
    std::string entry = Narrow(p);
    size_t eq = entry.find('=', 1);  // entries like "=C:=C:\" start with '='
    if (entry.empty() || entry[0] == '=' || eq == std::string::npos) continue;
    env[entry.substr(0, eq)] = entry.substr(eq + 1);
  }
  FreeEnvironmentStringsW(block);
  return env;
}

int Run(const Command& cmd, const LineFn& on_line, const std::atomic<bool>* cancel, std::string* error) {
  std::wstring cmdline = Widen(cmd.raw_cmdline.empty() ? JoinArgs(cmd.args) : cmd.raw_cmdline);
  if (cmdline.empty()) {
    if (error) *error = "nothing to run";
    return kStartFailed;
  }

  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof sa;
  sa.bInheritHandle = TRUE;
  HANDLE read_end = nullptr, write_end = nullptr;
  if (!CreatePipe(&read_end, &write_end, &sa, 0)) {
    if (error) *error = "CreatePipe failed: " + ErrorText(GetLastError());
    return kStartFailed;
  }
  SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
  HANDLE null_in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

  STARTUPINFOW si{};
  si.cb = sizeof si;
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = null_in == INVALID_HANDLE_VALUE ? nullptr : null_in;
  si.hStdOutput = write_end;
  si.hStdError = write_end;

  std::wstring env_block;
  if (cmd.env) env_block = EnvBlock(*cmd.env);
  std::wstring cwd = Widen(cmd.cwd);

  PROCESS_INFORMATION pi{};
  const DWORD flags = CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED;
  BOOL ok = CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, TRUE, flags,
                           cmd.env ? static_cast<LPVOID>(env_block.data()) : nullptr,
                           cmd.cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
  DWORD create_error = ok ? 0 : GetLastError();
  CloseHandle(write_end);
  if (null_in != INVALID_HANDLE_VALUE) CloseHandle(null_in);
  if (!ok) {
    CloseHandle(read_end);
    if (error) *error = "Could not start " + ProgramName(cmd) + ": " + ErrorText(create_error);
    return kStartFailed;
  }

  // Everything the program starts (ninja, clang, ...) joins this job, so cancelling
  // or closing the launcher stops the whole build.
  HANDLE job = CreateJobObjectW(nullptr, nullptr);
  if (job) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof info);
    if (!AssignProcessToJobObject(job, pi.hProcess)) {
      CloseHandle(job);
      job = nullptr;
    }
  }
  ResumeThread(pi.hThread);
  CloseHandle(pi.hThread);

  LineSplitter splitter(on_line);
  char buf[8192];
  bool cancelled = false;
  auto drain = [&]() {
    DWORD avail = 0;
    while (PeekNamedPipe(read_end, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
      DWORD got = 0;
      if (!ReadFile(read_end, buf, std::min<DWORD>(avail, sizeof buf), &got, nullptr) || got == 0) break;
      splitter.Feed(buf, got);
    }
  };
  for (;;) {
    DWORD avail = 0;
    if (!PeekNamedPipe(read_end, nullptr, 0, nullptr, &avail, nullptr)) break;  // all writers closed
    if (avail > 0) {
      DWORD got = 0;
      if (!ReadFile(read_end, buf, std::min<DWORD>(avail, sizeof buf), &got, nullptr) || got == 0) break;
      splitter.Feed(buf, got);
      continue;
    }
    if (cancel && cancel->load()) {
      cancelled = true;
      break;
    }
    if (WaitForSingleObject(pi.hProcess, 15) == WAIT_OBJECT_0) {
      // Finished. Helpers it left running may hold the pipe open, so do not wait for
      // the pipe to close: take what is buffered and stop.
      drain();
      break;
    }
  }
  splitter.Flush();

  int code = 0;
  if (cancelled) {
    if (job)
      TerminateJobObject(job, 1);
    else
      TerminateProcess(pi.hProcess, 1);
    WaitForSingleObject(pi.hProcess, 10000);
    code = kCancelled;
  } else {
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    code = static_cast<int>(exit_code);
  }
  CloseHandle(pi.hProcess);
  CloseHandle(read_end);
  if (job) CloseHandle(job);
  return code;
}

Child::~Child() {
  if (handle_) CloseHandle(static_cast<HANDLE>(handle_));
}

bool Child::Start(const Command& cmd, std::string* error) {
  if (handle_) {
    CloseHandle(static_cast<HANDLE>(handle_));
    handle_ = nullptr;
  }
  std::wstring cmdline = Widen(cmd.raw_cmdline.empty() ? JoinArgs(cmd.args) : cmd.raw_cmdline);
  std::wstring cwd = Widen(cmd.cwd);
  std::wstring env_block;
  if (cmd.env) env_block = EnvBlock(*cmd.env);
  STARTUPINFOW si{};
  si.cb = sizeof si;
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT,
                      cmd.env ? static_cast<LPVOID>(env_block.data()) : nullptr,
                      cmd.cwd.empty() ? nullptr : cwd.c_str(), &si, &pi)) {
    if (error) *error = "Could not start " + ProgramName(cmd) + ": " + ErrorText(GetLastError());
    return false;
  }
  CloseHandle(pi.hThread);
  handle_ = pi.hProcess;
  running_ = true;
  exit_code_ = 0;
  return true;
}

bool Child::Running() {
  if (!running_ || !handle_) return false;
  if (WaitForSingleObject(static_cast<HANDLE>(handle_), 0) == WAIT_OBJECT_0) {
    DWORD code = 0;
    GetExitCodeProcess(static_cast<HANDLE>(handle_), &code);
    exit_code_ = static_cast<int>(code);
    running_ = false;
  }
  return running_;
}

bool OpenWithSystem(const std::string& path_or_url) {
  std::wstring target = Widen(path_or_url);
  auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
  return result > 32;
}

int RunShell(const std::string& exe, const std::string& params, const std::atomic<bool>* cancel,
             std::string* error) {
  std::wstring wexe = Widen(exe), wparams = Widen(params);
  SHELLEXECUTEINFOW info{};
  info.cbSize = sizeof info;
  info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
  info.lpVerb = L"open";
  info.lpFile = wexe.c_str();
  info.lpParameters = wparams.empty() ? nullptr : wparams.c_str();
  info.nShow = SW_SHOWNORMAL;
  if (!ShellExecuteExW(&info) || !info.hProcess) {
    DWORD err = GetLastError();
    if (error) *error = err == ERROR_CANCELLED ? "the administrator prompt was declined" : ErrorText(err);
    return kStartFailed;
  }
  for (;;) {
    if (WaitForSingleObject(info.hProcess, 200) == WAIT_OBJECT_0) break;
    if (cancel && cancel->load()) {
      CloseHandle(info.hProcess);
      return kCancelled;  // the installer keeps running on its own
    }
  }
  DWORD code = 1;
  GetExitCodeProcess(info.hProcess, &code);
  CloseHandle(info.hProcess);
  return static_cast<int>(code);
}

#else
// ===================================================================================
// Linux / Steam Deck / macOS: SDL's process API
// ===================================================================================

namespace {

SDL_Process* CreateSdlProcess(const Command& cmd, bool pipe_output) {
  std::vector<const char*> argv;
  for (const auto& a : cmd.args) argv.push_back(a.c_str());
  argv.push_back(nullptr);
  SDL_PropertiesID props = SDL_CreateProperties();
  SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, argv.data());
  SDL_Environment* env = nullptr;
  if (cmd.env) {
    env = SDL_CreateEnvironment(false);
    for (const auto& [k, v] : *cmd.env) SDL_SetEnvironmentVariable(env, k.c_str(), v.c_str(), true);
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ENVIRONMENT_POINTER, env);
  }
  if (!cmd.cwd.empty()) SDL_SetStringProperty(props, SDL_PROP_PROCESS_CREATE_WORKING_DIRECTORY_STRING, cmd.cwd.c_str());
  if (pipe_output) {
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
  }
  SDL_Process* p = SDL_CreateProcessWithProperties(props);
  SDL_DestroyProperties(props);
  if (env) SDL_DestroyEnvironment(env);
  return p;
}

}  // namespace

Env CurrentEnvironment() {
  Env env;
  for (char** e = environ; e && *e; ++e) {
    std::string entry = *e;
    size_t eq = entry.find('=');
    if (eq == std::string::npos || eq == 0) continue;
    env[entry.substr(0, eq)] = entry.substr(eq + 1);
  }
  return env;
}

int Run(const Command& cmd, const LineFn& on_line, const std::atomic<bool>* cancel, std::string* error) {
  if (cmd.args.empty()) {
    if (error) *error = "nothing to run";
    return kStartFailed;
  }
  SDL_Process* p = CreateSdlProcess(cmd, true);
  if (!p) {
    if (error) *error = "Could not start " + cmd.args[0] + ": " + SDL_GetError();
    return kStartFailed;
  }
  SDL_IOStream* out = SDL_GetProcessOutput(p);
  LineSplitter splitter(on_line);
  char buf[8192];
  bool cancelled = false;
  while (out) {
    size_t n = SDL_ReadIO(out, buf, sizeof buf);
    if (n > 0) {
      splitter.Feed(buf, n);
      continue;
    }
    SDL_IOStatus status = SDL_GetIOStatus(out);
    if (status == SDL_IO_STATUS_EOF || status == SDL_IO_STATUS_ERROR) break;
    if (cancel && cancel->load()) {
      cancelled = true;
      SDL_KillProcess(p, true);
      break;
    }
    SDL_Delay(10);
  }
  splitter.Flush();
  int code = 0;
  SDL_WaitProcess(p, true, &code);
  SDL_DestroyProcess(p);
  return cancelled ? kCancelled : code;
}

Child::~Child() {
  if (handle_) SDL_DestroyProcess(static_cast<SDL_Process*>(handle_));
}

bool Child::Start(const Command& cmd, std::string* error) {
  if (handle_) {
    SDL_DestroyProcess(static_cast<SDL_Process*>(handle_));
    handle_ = nullptr;
  }
  SDL_Process* p = cmd.args.empty() ? nullptr : CreateSdlProcess(cmd, false);
  if (!p) {
    if (error) *error = std::string("Could not start the game: ") + SDL_GetError();
    return false;
  }
  handle_ = p;
  running_ = true;
  exit_code_ = 0;
  return true;
}

bool Child::Running() {
  if (!running_ || !handle_) return false;
  int code = 0;
  if (SDL_WaitProcess(static_cast<SDL_Process*>(handle_), false, &code)) {
    exit_code_ = code;
    running_ = false;
  }
  return running_;
}

bool OpenWithSystem(const std::string& path_or_url) {
  std::string url = path_or_url;
  if (url.find("://") == std::string::npos) url = "file://" + url;
  return SDL_OpenURL(url.c_str());
}

int RunShell(const std::string& exe, const std::string& params, const std::atomic<bool>* cancel,
             std::string* error) {
  Command cmd;
  cmd.args = {exe};
  if (!params.empty()) cmd.args.push_back(params);
  return Run(cmd, nullptr, cancel, error);
}

#endif

}  // namespace proc
