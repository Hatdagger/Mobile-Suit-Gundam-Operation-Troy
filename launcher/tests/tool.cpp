// Command-line checks for the launcher's building blocks (not shipped).
//   optroy_launcher_tool extract <game copy> <out dir>
//   optroy_launcher_tool find <folder>...
//   optroy_launcher_tool run <program> [args...]
//   optroy_launcher_tool tools
//   optroy_launcher_tool name <settings .toml> <text>   (types <text> key by key, saves it as the player name)
//   optroy_launcher_tool english <launcher folder> apply|remove|status|extracted
//       (English patch from "<launcher folder>/English Patch"; "extracted" = what happens after extracting)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <SDL3/SDL.h>

#include "english_patch.h"
#include "game_config.h"
#include "game_finder.h"
#include "lan_discovery.h"
#include "lobby_server.h"
#include "gdf.h"
#include "player_name.h"
#include "subprocess.h"
#include "setup_steps.h"
#include "toolchain.h"

int main(int argc, char** argv) {
  if (argc < 2) return 2;
  const std::string cmd = argv[1];
  if (cmd == "lan-server" && argc >= 3) {
    // optroy_launcher_tool lan-server <port> [discovery port] [seconds]
    lobby::Server server;
    std::string err;
    const uint16_t dport = argc >= 4 ? static_cast<uint16_t>(std::atoi(argv[3])) : lan::kDiscoveryPort;
    if (!server.Start(static_cast<uint16_t>(std::atoi(argv[2])), "Test Host", &err, dport)) {
      std::printf("start failed: %s\n", err.c_str());
      return 1;
    }
    std::printf("LAN server on port %s\n", argv[2]);
    std::fflush(stdout);
    const int seconds = argc >= 5 ? std::atoi(argv[4]) : 3600;
    for (int i = 0; i < seconds * 10; ++i) SDL_Delay(100);
    auto st = server.status();
    std::printf("players=%zu matches=%d requests=%llu\n", st.players.size(), st.matches,
                (unsigned long long)st.requests);
    server.Stop();
    return 0;
  }
  if (cmd == "lan-scan" && argc >= 3) {
    // optroy_launcher_tool lan-scan <discovery port> [target address]
    lan::Scanner scanner;
    scanner.Start(static_cast<uint16_t>(std::atoi(argv[2])), argc >= 4 ? argv[3] : "");
    SDL_Delay(2500);
    auto hosts = scanner.hosts();
    scanner.Stop();
    for (const auto& h : hosts)
      std::printf("host name=[%s] address=%s port=%u players=%d matches=%d version=%s\n", h.name.c_str(),
                  h.address.c_str(), h.port, h.players, h.matches, h.version.c_str());
    std::printf("hosts=%zu\n", hosts.size());
    return hosts.empty() ? 1 : 0;
  }
  if (cmd == "extract" && argc == 4) {
    gdf::Source src;
    std::string err;
    if (!gdf::Open(util::U8Path(argv[2]), src, err)) { std::printf("open failed: %s\n", err.c_str()); return 1; }
    std::vector<gdf::Entry> entries;
    if (!gdf::List(src, entries, err)) std::printf("list warning: %s\n", err.c_str());
    std::printf("kind=%s entries=%zu title=%08X\n", gdf::KindName(src.kind), entries.size(), gdf::ReadTitleId(src, entries));
    gdf::Progress result;
    if (!gdf::Extract(src, entries, util::U8Path(argv[3]), true, nullptr, nullptr, err, &result)) {
      std::printf("extract failed: %s\n", err.c_str());
      return 1;
    }
    std::printf("extracted files=%zu bytes=%llu\n", result.done_files, (unsigned long long)result.done_bytes);
    return 0;
  }
  if (cmd == "find" && argc >= 3) {
    std::vector<fs::path> roots;
    for (int i = 2; i < argc; ++i) roots.push_back(util::U8Path(argv[i]));
    finder::Candidate c;
    if (finder::Search(roots, {}, nullptr, c))
      std::printf("found %s kind=%s title=%08X files=%zu bytes=%llu\n", util::PathStr(c.path).c_str(),
                  gdf::KindName(c.kind), c.title_id, c.file_count, (unsigned long long)c.total_bytes);
    else
      std::printf("not found\n");
    return 0;
  }
  if (cmd == "run" && argc >= 3) {
    proc::Command c;
    for (int i = 2; i < argc; ++i) c.args.push_back(argv[i]);
    std::string err;
    int code = proc::Run(c, [](std::string_view line) { std::printf("line: [%.*s]\n", int(line.size()), line.data()); },
                         nullptr, &err);
    std::printf("exit=%d err=%s\n", code, err.c_str());
    return 0;
  }
  if (cmd == "english" && argc == 4) {
    util::Log log;
    std::atomic<bool> cancel{false};
    JobContext ctx;
    ctx.layout = DetectLayout(util::U8Path(argv[2]));
    ctx.log = &log;
    ctx.cancel = &cancel;
    ctx.stage = [](const std::string& s) { std::printf("stage: %s\n", s.c_str()); };
    const std::string what = argv[3];
    bool ok = true;
    if (what == "apply") ok = english_patch::Apply(ctx, toolchain::Info{});
    else if (what == "remove") ok = english_patch::Remove(ctx);
    else if (what == "extracted") ok = english_patch::AfterExtract(ctx, toolchain::Info{});
    for (const std::string& line : log.Range(0, log.Count())) std::printf("log: %s\n", line.c_str());
    for (const std::string& line : ctx.error_lines) std::printf("problem: %s\n", line.c_str());
    const english_patch::Status s = english_patch::GetStatus(ctx.layout);
    std::printf("ok=%d error=%s patches=%zu archives=%zu applied=%d files=%zu\n", ok, ctx.error.c_str(),
                s.patch_files, s.archives, s.applied, s.applied_files);
    return ok ? 0 : 1;
  }
  if (cmd == "build" && argc == 3) {
    util::Log log;
    std::atomic<bool> cancel{false};
    JobContext ctx;
    ctx.layout = DetectLayout(util::U8Path(argv[2]));
    ctx.log = &log;
    ctx.cancel = &cancel;
    std::string last_stage;
    float last_p = -2;
    ctx.stage = [&](const std::string& s) {
      if (s != last_stage) std::printf("stage: %s\n", s.c_str());
      last_stage = s;
    };
    ctx.progress = [&](float p) {
      if (p != last_p) std::printf("progress: %.3f\n", p);
      last_p = p;
    };
    auto tools = toolchain::Detect(ctx.layout.tools_dir);
    const bool ok = steps::BuildGame(ctx, tools);
    std::printf("dev=%d ok=%d error=%s exe=%s built=%d\n", ctx.layout.dev, ok, ctx.error.c_str(),
                util::PathStr(ctx.layout.GameExe()).c_str(), steps::BuildReady(ctx.layout));
    for (size_t i = 0; i < log.Count(); ++i)
      if (log.Range(i, 1)[0].rfind("> ", 0) == 0) std::printf("log: %s\n", log.Range(i, 1)[0].c_str());
    return ok ? 0 : 1;
  }
  if (cmd == "source" && argc == 3) {
    util::Log log;
    std::atomic<bool> cancel{false};
    JobContext ctx;
    ctx.layout = DetectLayout(util::U8Path(argv[2]));
    ctx.log = &log;
    ctx.cancel = &cancel;
    int last_pct = -1;
    ctx.stage = [](const std::string& s) { std::printf("stage: %s\n", s.c_str()); };
    ctx.progress = [&](float p) {
      const int pct = int(p * 100);
      if (p >= 0 && pct / 10 != last_pct / 10) std::printf("progress: %d%%\n", pct);
      last_pct = pct;
    };
    auto tools = toolchain::Detect(ctx.layout.tools_dir);
    const bool ok = steps::GetSource(ctx, tools);
    std::printf("dev=%d ok=%d error=%s source=%s ready=%d libs=%d\n", ctx.layout.dev, ok, ctx.error.c_str(),
                util::PathStr(ctx.layout.source_dir).c_str(), steps::SourceReady(ctx.layout),
                steps::HelperLibsReady(ctx.layout));
    return ok ? 0 : 1;
  }
  if (cmd == "tools") {
    auto info = toolchain::Detect(util::U8Path("/tmp/optroy-tools"));
    std::printf("ok=%d summary=%s\nclang=%s cmake=%s ninja=%s git=%s curl=%s tar=%s\n", info.ok, info.Summary().c_str(),
                info.clang_dir.c_str(), info.cmake.c_str(), info.ninja.c_str(), info.git.c_str(), info.curl.c_str(),
                info.tar.c_str());
    proc::Env env;
    std::string err;
    std::printf("env=%d %s PATH starts: %.80s\n", toolchain::BuildEnvironment(info, env, err), err.c_str(), env["PATH"].c_str());
    return 0;
  }
  if (cmd == "name" && argc == 4) {
    std::string typed;
    for (const char* p = argv[3]; *p; ++p) typed = player_name::Type(typed, *p);
    const std::string clean = player_name::Clean(typed);
    std::printf("typed=[%s] clean=[%s]\n", typed.c_str(), clean.c_str());
    if (clean.empty()) return 1;
    GameConfig config;
    config.Load(util::U8Path(argv[2]));
    config.SetString(player_name::kConfigKey, clean);
    if (!config.Save()) return 1;
    GameConfig check;
    check.Load(util::U8Path(argv[2]));
    std::printf("read back=[%s]\n", check.GetString(player_name::kConfigKey, player_name::kDefault).c_str());
    return 0;
  }
  return 2;
}
