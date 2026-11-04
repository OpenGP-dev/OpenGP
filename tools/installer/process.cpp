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
#include <cerrno>
#include <fcntl.h>
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

ProcessResult run_process(const std::vector<std::string>& args, const std::string& cwd) {
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
  // Only this pipe is inherited: with several compilers started at once, each
  // would otherwise also inherit the others' pipes, and a pipe's reader would
  // not see the end of its output until every one of them had finished.
  SIZE_T attr_size = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
  std::vector<char> attr_buffer(attr_size);
  auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buffer.data());
  HANDLE inherited[] = {write_end};
  const bool have_attrs = InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size) &&
                          UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited,
                                                    sizeof(inherited), nullptr, nullptr);
  STARTUPINFOEXW si{};
  si.StartupInfo.cb = sizeof(si);
  si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  si.StartupInfo.hStdInput = nullptr;
  si.StartupInfo.hStdOutput = write_end;
  si.StartupInfo.hStdError = write_end;
  if (have_attrs) si.lpAttributeList = attrs;
  const std::wstring wide_cwd = widen(cwd);
  PROCESS_INFORMATION pi{};
  const BOOL ok = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                                 CREATE_NO_WINDOW | (have_attrs ? EXTENDED_STARTUPINFO_PRESENT : 0), nullptr,
                                 cwd.empty() ? nullptr : wide_cwd.c_str(), &si.StartupInfo, &pi);
  if (have_attrs) DeleteProcThreadAttributeList(attrs);
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
  // The pipe is made close-on-exec, and the spawn that dup2s it into place
  // happens under a lock: a compiler started at the same moment from another
  // thread never inherits this one's pipe (see the Windows case).
  static std::mutex spawn_mutex;
  std::unique_lock spawn_lock(spawn_mutex);
  int fds[2];
  if (pipe(fds) != 0) return result;
  fcntl(fds[0], F_SETFD, FD_CLOEXEC);
  fcntl(fds[1], F_SETFD, FD_CLOEXEC);
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_adddup2(&actions, fds[1], 1);
  posix_spawn_file_actions_adddup2(&actions, fds[1], 2);
  // macOS 26 deprecates the _np name for POSIX 2024's addchdir, but the
  // packages run on macOS 13 and later, which have only the _np one.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
  if (!cwd.empty()) posix_spawn_file_actions_addchdir_np(&actions, cwd.c_str());
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
  std::vector<char*> argv;
  for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
  argv.push_back(nullptr);
  pid_t pid = 0;
  const int err = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  close(fds[1]);
  spawn_lock.unlock();
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
  pid_t waited;
  while ((waited = waitpid(pid, &status, 0)) < 0 && errno == EINTR) {
  }
  result.exit_code = waited == pid && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
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
