// SPDX-License-Identifier: GPL-3.0-or-later

#include "installer.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <chrono>
#include <thread>

#include "analysis.h"
#include "app/paths.h"
#include "codegen.h"
#include "config.h"
#include "disc/disc.h"
#include "disc/sha1.h"
#include "dol.h"
#include "os/atomic_file.h"
#include "process.h"
#include "sha256.h"
#include "symbols.h"

#define TOML_EXCEPTIONS 0
#include <toml.hpp>

namespace installer {

namespace {

using os::utf8;

// The last part of a program's output, for an error message.
std::string tail(const std::string& output, std::size_t max = 600) {
  return output.size() <= max ? output : "..." + output.substr(output.size() - max);
}

std::vector<std::string> strings(const toml::node* node) {
  std::vector<std::string> out;
  if (const toml::array* a = node ? node->as_array() : nullptr)
    for (const toml::node& n : *a)
      if (auto s = n.value<std::string>()) out.push_back(*s);
  return out;
}

bool parse_file(const fs::path& path, toml::table* table, std::string* error) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    if (error) *error = "cannot read " + utf8(path);
    return false;
  }
  const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  toml::parse_result parsed = toml::parse(text);
  if (!parsed) {
    if (error) *error = utf8(path) + ": " + std::string(parsed.error().description());
    return false;
  }
  *table = std::move(parsed).table();
  return true;
}

bool works(const std::string& cxx) { return run_process({cxx, "--version"}).exit_code == 0; }

// Whether `cxx` is GCC 12 or Clang 16 or later (Apple's Clang counts by its
// own numbers and is not checked): compiles a probe.
bool new_enough(const Package& package, const std::string& cxx) {
  const fs::path probe = package.home / "build" / "compiler-probe.cpp";
  const std::string text =
      "#if !defined(__apple_build_version__) && defined(__clang__) && __clang_major__ < 16\n#error Clang 16\n"
      "#elif !defined(__clang__) && defined(__GNUC__) && __GNUC__ < 12\n#error GCC 12\n#endif\n";
  if (!os::write_file_atomically(utf8(probe), text.data(), text.size())) return true;  // cannot tell
  const bool ok = run_process({cxx, "-std=c++20", "-fsyntax-only", utf8(probe)}).exit_code == 0;
  std::error_code ec;
  fs::remove(probe, ec);
  return ok;
}

#ifdef _WIN32
// The copies of curl and tar that come with Windows 10 and later (tar is
// bsdtar, which reads zip files).
std::string system_tool(const char* name) {
  const char* root = std::getenv("SystemRoot");
  return std::string(root ? root : "C:\\Windows") + "\\System32\\" + name + ".exe";
}
#else
std::string system_tool(const char* name) { return name; }
#endif

}  // namespace

bool load_package(const fs::path& home, Package* package, std::string* error) {
  Package p;
  p.home = home;
  p.sdk = home / "sdk";
  toml::table t;
  if (!parse_file(p.sdk / "package.toml", &t, error)) return false;
  p.version = t["version"].value_or(std::string());
  const auto tc = t["toolchain"];
  p.toolchain_url = tc["url"].value_or(std::string());
  p.toolchain_sha256 = tc["sha256"].value_or(std::string());
  p.toolchain_dir = tc["dir"].value_or(std::string());
  p.toolchain_cxx = tc["cxx"].value_or(std::string());
  p.toolchain_exclude = strings(tc["exclude"].node());
  const auto build = t["build"];
  p.cxx_candidates = strings(build["cxx"].node());
  p.cxx_flags = strings(build["cxx_flags"].node());
  p.link_flags = strings(build["link_flags"].node());
  p.libs = strings(build["libs"].node());
  p.system_libs = strings(build["system_libs"].node());
  p.executable_suffix = build["executable_suffix"].value_or(std::string());
  p.link_group = build["link_group"].value_or(true);
  p.common_sources = strings(build["common_sources"].node());
  p.game_sources = strings(build["game_sources"].node());
  if (p.common_sources.empty() || p.game_sources.empty()) {
    if (error) *error = utf8(p.sdk / "package.toml") + ": no game sources listed";
    return false;
  }
  for (const std::string& dir : strings(t["games"].node())) {
    GameInfo g;
    g.dir = p.sdk / "games" / dir;
    toml::table m;
    if (!parse_file(g.dir / "game.toml", &m, error)) return false;
    g.id = m["id"].value_or(std::string());
    g.title = m["title"].value_or(std::string());
    g.dol_sha1 = m["dol_sha1"].value_or(std::string());
    g.executable = m["executable"].value_or(std::string());
    p.games.push_back(std::move(g));
  }
  if (p.games.empty()) {
    if (error) *error = utf8(p.sdk / "package.toml") + ": no games";
    return false;
  }
  *package = std::move(p);
  return true;
}

bool find_compiler(const Package& package, const std::string& requested, const Reporter& report, std::string* cxx,
                   std::string* error) {
  if (!requested.empty()) {
    if (works(requested)) {
      *cxx = requested;
      return true;
    }
    if (error) *error = "cannot run the compiler " + requested;
    return false;
  }
  if (!package.toolchain_url.empty()) {
    const fs::path dir = package.home / "toolchain";
    const fs::path compiler = dir / package.toolchain_dir / package.toolchain_cxx;
    std::error_code ec;
    if (!fs::exists(compiler, ec)) {
      // curl and tar run inside the toolchain folder with relative names:
      // Windows' tar reads its arguments in the system's code page, so a
      // package folder named with other characters would not reach it.
      fs::create_directories(dir, ec);
      // Named after the toolchain, so a download interrupted for one version
      // is never resumed with another's bytes.
      const std::string archive_name = package.toolchain_dir + ".zip";
      const fs::path archive = dir / archive_name;
      report.line("Downloading the compiler (once, about 190 MB): " + package.toolchain_url);
      // -C - resumes a download that was interrupted.
      const ProcessResult download = run_process(
          {system_tool("curl"), "-fL", "--retry", "3", "-C", "-", "-o", archive_name, package.toolchain_url},
          utf8(dir));
      if (download.exit_code != 0) {
        if (error)
          *error = "cannot download " + package.toolchain_url + " (check the internet connection and run OpenGP again):\n" +
                   tail(download.output);
        return false;
      }
      report.line("Checking the download");
      const std::string sha = sha256_file(utf8(archive));
      if (sha != package.toolchain_sha256) {
        fs::remove(archive, ec);
        if (error) *error = "the download's SHA-256 is " + sha + ", expected " + package.toolchain_sha256;
        return false;
      }
      // Unpacked into a folder of its own first, and moved into place only
      // when complete: an interrupted unpack is never taken for a compiler.
      report.line("Unpacking the compiler");
      const fs::path unpacking = dir / "unpacking";
      fs::remove_all(unpacking, ec);
      fs::create_directories(unpacking, ec);
      std::vector<std::string> tar = {system_tool("tar"), "-xf", archive_name, "-C", "unpacking"};
      for (const std::string& e : package.toolchain_exclude) tar.insert(tar.end(), {"--exclude", e});
      const ProcessResult r = run_process(tar, utf8(dir));
      std::string why = r.exit_code != 0 ? tail(r.output) : std::string();
      if (r.exit_code == 0) {
        fs::remove_all(dir / package.toolchain_dir, ec);
        // A virus scanner looking at the new files can hold the folder for a
        // moment, which makes Windows refuse to move it: try for a while.
        for (int attempt = 0; attempt < 40; ++attempt) {
          fs::rename(unpacking / package.toolchain_dir, dir / package.toolchain_dir, ec);
          if (!ec) break;
          std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        if (ec) why = "cannot move it into place: " + ec.message();
      }
      fs::remove_all(unpacking, ec);
      if (!fs::exists(compiler, ec)) {
        // The download is kept (checked): the next run only unpacks again.
        if (error) *error = "cannot unpack the compiler into " + utf8(dir) + ":\n" + why;
        return false;
      }
      fs::remove(archive, ec);
    }
    *cxx = utf8(compiler);
    return true;
  }
  std::string too_old;
  for (const std::string& c : package.cxx_candidates) {
    if (!works(c)) continue;
    if (!new_enough(package, c)) {
      if (too_old.empty()) too_old = c;
      continue;
    }
    *cxx = c;
    return true;
  }
  if (error) {
    *error = "no C++ compiler found (tried";
    for (const std::string& c : package.cxx_candidates) *error += " " + c;
#ifdef __APPLE__
    *error += "). Install Apple's command line tools: xcode-select --install";
#else
    *error += "). Install GCC 12 or later, or Clang 16 or later: on Ubuntu 22.04, sudo apt install g++-12; on "
              "newer systems, sudo apt install g++.";
    if (!too_old.empty()) *error += " (" + too_old + " is too old.)";
#endif
  }
  return false;
}

const GameInfo* identify_image(const Package& package, const std::string& image) {
  disc::Image img;
  if (!img.open(image, nullptr)) return nullptr;
  for (const GameInfo& g : package.games)
    if (g.id == img.game_id()) return &g;
  // A placeholder header (some converted dumps say "RELSAB"): go by the executable.
  const std::string sha1 = disc::dol_sha1(img);
  for (const GameInfo& g : package.games)
    if (!sha1.empty() && g.dol_sha1 == sha1) return &g;
  return nullptr;
}

// Why an image is not one of the package's games, for the player.
std::string why_not_supported(const Package& package, const std::string& image) {
  disc::Image img;
  if (!img.open(image, nullptr)) return "not a GameCube or Triforce disc image; skipped";
  std::string supported;
  for (const GameInfo& g : package.games) supported += (supported.empty() ? "" : " or ") + g.id;
  return "its game is " + img.game_id() + ", not a supported release (OpenGP needs " + supported +
         ", the USA releases); skipped";
}

std::vector<std::pair<const GameInfo*, std::string>> find_images(const Package& package,
                                                                   const std::vector<std::string>& given,
                                                                   const Reporter& report) {
  std::vector<std::pair<const GameInfo*, std::string>> found;
  auto have = [&](const GameInfo* g) {
    return std::any_of(found.begin(), found.end(), [&](const auto& f) { return f.first == g; });
  };
  if (!given.empty()) {
    for (const std::string& name : given) {
      // Remembered as an absolute path: the game may be started from anywhere.
      std::error_code ec;
      const fs::path absolute = fs::absolute(os::utf8_path(name), ec);
      const std::string image = ec ? name : utf8(absolute.lexically_normal());
      const GameInfo* g = identify_image(package, image);
      if (!g) report.line(image + ": " + why_not_supported(package, image));
      else if (!have(g)) found.emplace_back(g, image);
    }
    return found;
  }
  const fs::path user_dir = package.home / "user";
  for (const GameInfo& g : package.games) {
    const std::string image = app::remembered_image(user_dir, g.id);
    std::error_code ec;
    if (!image.empty() && fs::is_regular_file(os::utf8_path(image), ec) && identify_image(package, image) == &g)
      found.emplace_back(&g, image);
  }
  for (const char* folder : {"ISO", "private"}) {
    std::error_code ec;
    const fs::path dir = package.home / folder;
    if (!fs::is_directory(dir, ec)) continue;
    // Never throws: an unreadable folder in there is skipped.
    for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
      std::string ext = utf8(it->path().extension());
      std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
      const std::string image = utf8(it->path());
      const std::string name = utf8(it->path().filename());
      if (ext == ".rvz" || ext == ".gcz" || ext == ".wia" || ext == ".ciso" || ext == ".wbfs") {
        report.line(name + ": a compressed image; convert it to .iso with Dolphin (see ISO/README.txt)");
        continue;
      }
      if (ext != ".iso" && ext != ".gcm") continue;
      if (const GameInfo* g = identify_image(package, image)) {
        if (!have(g)) found.emplace_back(g, image);
      } else {
        report.line(name + ": " + why_not_supported(package, image));
      }
    }
  }
  // The package's order: the main game first.
  std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  return found;
}

bool is_installed(const Package& package, const GameInfo& game) {
  const fs::path exe = package.home / (game.executable + package.executable_suffix);
  std::error_code ec;
  if (!fs::exists(exe, ec)) return false;
  const ProcessResult r = run_process({utf8(exe), "--version"});
  return r.exit_code == 0 && r.output.rfind("OpenGP " + package.version + ":", 0) == 0;
}

bool install_game(const Package& package, const GameInfo& game, const std::string& image, const std::string& cxx,
                  const BuildOptions& options, const Reporter& report, std::string* error) {
  auto fail = [&](const std::string& msg) {
    if (error) *error = msg;
    return false;
  };
  auto cancelled = [&] { return options.cancel && options.cancel->load(); };
  const fs::path work = package.home / "build" / game.id;
  std::error_code ec;
  fs::remove_all(work, ec);
  fs::create_directories(work / "obj", ec);

  // 1. The executable, from the image, checked against the supported release.
  report.line("Reading " + image);
  disc::Image img;
  std::string err;
  if (!img.open(image, &err)) return fail(err);
  std::vector<gekko::u8> dol;
  if (!img.read_dol(&dol)) return fail(image + ": cannot read its main.dol");
  const std::string sha1 = disc::to_hex(disc::sha1(dol.data(), dol.size()));
  if (sha1 != game.dol_sha1)
    return fail(image + ": its main.dol (SHA-1 " + sha1 + ") is not the supported release of " + game.title +
                " (SHA-1 " + game.dol_sha1 + ")");
  if (img.game_id() != game.id)
    report.line("Its header says " + img.game_id() + ", but it holds " + game.id + "'s executable: fine.");
  const fs::path dol_path = work / "main.dol";
  if (!os::write_file_atomically(utf8(dol_path), dol.data(), dol.size())) return fail("cannot write " + utf8(dol_path));

  // 2. Recompile it.
  if (cancelled()) return fail("cancelled");
  report.line("Recompiling " + game.title);
  recomp::Config config;
  if (!config.load(utf8(game.dir / "recomp.toml"), &err)) return fail(err);
  config.dol_path = utf8(dol_path);
  config.symbols_path = utf8(game.dir / "config" / "symbols.txt");
  config.output_directory = utf8(work / "generated");
  recomp::Dol rdol;
  recomp::SymbolFile symbols;
  if (!rdol.load(config.dol_path, &err) || !symbols.load(config.symbols_path, &err)) return fail(err);
  recomp::Program program;
  if (!program.build(rdol, &symbols, config, &err)) return fail(err);
  if (!recomp::generate(program, config.output_directory, nullptr, &err)) return fail(err);
  fs::remove(dol_path, ec);
  if (cancelled()) return fail("cancelled");

  // 3. Compile: the hand-written sources first (quick, so problems show at once),
  // then the generated ones.
  const fs::path common = package.sdk / "games" / "common";
  std::vector<fs::path> sources;
  for (const std::string& s : package.common_sources) sources.push_back(common / s);
  for (const std::string& s : package.game_sources) sources.push_back(game.dir / s);
  std::vector<fs::path> generated;
  for (const auto& entry : fs::directory_iterator(work / "generated"))
    if (entry.path().extension() == ".cpp") generated.push_back(entry.path());
  std::sort(generated.begin(), generated.end());
  sources.insert(sources.end(), generated.begin(), generated.end());

  std::vector<std::string> base = {cxx};
  base.insert(base.end(), package.cxx_flags.begin(), package.cxx_flags.end());
  for (const fs::path& inc : {package.sdk / "include", work / "generated", common}) base.push_back("-I" + utf8(inc));
  base.push_back("-DOPENGP_GAME_ID=\"" + game.id + "\"");
  base.push_back("-DOPENGP_GAME_TITLE=\"" + game.title + "\"");
  base.push_back("-DOPENGP_DOL_SHA1=\"" + game.dol_sha1 + "\"");
  // Relative to the package folder, where the game's executable goes, so that
  // the package can be moved: forward slashes, which Windows accepts too.
  const std::u8string symbols_path =
      (game.dir / "config" / "symbols.txt").lexically_relative(package.home).generic_u8string();
  base.push_back("-DOPENGP_SYMBOLS=\"" + std::string(symbols_path.begin(), symbols_path.end()) + "\"");
  std::vector<std::vector<std::string>> commands;
  std::vector<std::string> objects;
  for (std::size_t i = 0; i < sources.size(); ++i) {
    const std::string obj = utf8(work / "obj" / (std::to_string(i) + "_" + utf8(sources[i].stem()) + ".o"));
    std::vector<std::string> c = base;
    c.insert(c.end(), {"-c", utf8(sources[i]), "-o", obj});
    commands.push_back(std::move(c));
    objects.push_back(obj);
  }
  const unsigned jobs = options.jobs ? options.jobs : std::max(1u, std::thread::hardware_concurrency());
  report.line("Compiling " + std::to_string(sources.size()) + " files (" + std::to_string(jobs) + " at a time)");
  std::size_t finished = 0;
  std::string compile_error;
  report.progress(0);
  const bool compiled = run_parallel(commands, jobs, [&](std::size_t i, const ProcessResult& r) {
    ++finished;
    report.progress(static_cast<double>(finished) / commands.size());
    if (r.exit_code != 0) {
      compile_error = utf8(sources[i]) + ":\n" + r.output;
      return false;
    }
    return !cancelled();
  });
  if (cancelled()) return fail("cancelled");
  if (!compiled) return fail("compiling failed: " + compile_error);

  // 4. Link, to a temporary name first (the game may be running).
  report.line("Linking");
  const fs::path exe = package.home / (game.executable + package.executable_suffix);
  const fs::path tmp = work / (game.executable + package.executable_suffix);
  std::vector<std::string> link = {cxx};
  link.insert(link.end(), objects.begin(), objects.end());
  link.insert(link.end(), package.link_flags.begin(), package.link_flags.end());
  if (package.link_group) link.push_back("-Wl,--start-group");
  for (const std::string& lib : package.libs) link.push_back(utf8(package.sdk / lib));
  if (package.link_group) link.push_back("-Wl,--end-group");
  link.insert(link.end(), package.system_libs.begin(), package.system_libs.end());
  link.insert(link.end(), {"-o", utf8(tmp)});
  if (const ProcessResult r = run_process(link); r.exit_code != 0) return fail("linking failed:\n" + r.output);
  fs::rename(tmp, exe, ec);
  if (ec) return fail("cannot replace " + utf8(exe) + " (is the game running?): " + ec.message());

  if (!app::remember_image(package.home / "user", game.id, image, &err)) report.line(err);
  if (!options.keep_build) fs::remove_all(work, ec);
  report.line(game.title + " is ready: " + utf8(exe));
  return true;
}

}  // namespace installer
