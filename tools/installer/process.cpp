// SPDX-License-Identifier: GPL-3.0-or-later

#include "process.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace installer {

namespace {

#ifdef _WIN32
std::wstring widen(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

// Quotes one argument the way CommandLineToArgvW (and the C runtime) splits them.
std::wstring quote(const std::wstring& arg) {
  if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;
  std::wstring out = L"\"";
  for (std::size_t i = 0;; ++i) {
    std::size_t backslashes = 0;
    while (i < arg.size() && arg[i] == L'\\') ++i, ++backslashes;
    if (i == arg.size()) {
      out.append(backslashes * 2, L'\\');
      break;
    }
    if (arg[i] == L'"') {
      out.append(backslashes * 2 + 1, L'\\');
      out += L'"';
    } else {
      out.append(backslashes, L'\\');
      out += arg[i];
    }
  }
  return out + L"\"";
}
#endif

}  // namespace

ProcessResult run_process(const std::vector<std::string>& args) {
  ProcessResult result;
  if (args.empty()) return result;
#ifdef _WIN32
  std::wstring command;
  for (const std::string& a : args) {
    if (!command.empty()) command += L' ';
    command += quote(widen(a));
  }
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  HANDLE read_end = nullptr, write_end = nullptr;
  if (!CreatePipe(&read_end, &write_end, &sa, 0)) return result;
  SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  si.hStdOutput = write_end;
  si.hStdError = write_end;
  PROCESS_INFORMATION pi{};
  const BOOL ok = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                 nullptr, &si, &pi);
  CloseHandle(write_end);
  if (!ok) {
    CloseHandle(read_end);
    result.output = "cannot start " + args[0];
    return result;
  }
  char buf[4096];
  DWORD n = 0;
  while (ReadFile(read_end, buf, sizeof(buf), &n, nullptr) && n > 0) result.output.append(buf, n);
  CloseHandle(read_end);
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 0;
  GetExitCodeProcess(pi.hProcess, &code);
  result.exit_code = static_cast<int>(code);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
#else
  int fds[2];
  if (pipe(fds) != 0) return result;
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, fds[1], 1);
  posix_spawn_file_actions_adddup2(&actions, fds[1], 2);
  posix_spawn_file_actions_addclose(&actions, fds[0]);
  posix_spawn_file_actions_addclose(&actions, fds[1]);
  std::vector<char*> argv;
  for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
  argv.push_back(nullptr);
  pid_t pid = 0;
  const int err = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  close(fds[1]);
  if (err != 0) {
    close(fds[0]);
    result.output = "cannot start " + args[0];
    return result;
  }
  char buf[4096];
  ssize_t n;
  while ((n = read(fds[0], buf, sizeof(buf))) > 0) result.output.append(buf, static_cast<std::size_t>(n));
  close(fds[0]);
  int status = 0;
  waitpid(pid, &status, 0);
  result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
  return result;
}

bool run_parallel(const std::vector<std::vector<std::string>>& commands, unsigned jobs,
                  const std::function<bool(std::size_t, const ProcessResult&)>& done) {
  std::atomic<std::size_t> next{0};
  std::atomic<bool> stop{false}, ok{true};
  std::mutex mutex;
  auto worker = [&] {
    for (;;) {
      if (stop) return;
      const std::size_t i = next++;
      if (i >= commands.size()) return;
      const ProcessResult r = run_process(commands[i]);
      std::lock_guard lock(mutex);
      if (r.exit_code != 0) ok = false;
      if (!done(i, r)) stop = true;
    }
  };
  std::vector<std::thread> threads;
  for (unsigned t = 0; t < std::max(1u, jobs); ++t) threads.emplace_back(worker);
  for (std::thread& t : threads) t.join();
  return ok && !stop && next >= commands.size();
}

}  // namespace installer
