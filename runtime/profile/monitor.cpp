// SPDX-License-Identifier: GPL-3.0-or-later

#include "profile/monitor.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "os/thread_priority.h"
#include "profile/json.h"
#include "profile/session.h"
#include "profile/symbols.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
// windows.h first
#include <intrin.h>
#include <psapi.h>
#include <timeapi.h>
#include <tlhelp32.h>
#else
#include <sys/resource.h>
#include <unistd.h>

#include <fstream>
#include <sstream>
#endif

namespace profile {

namespace fs = std::filesystem;

namespace {

constexpr std::size_t kMaxDepth = 64;
constexpr u64 kRecentUs = 10'000'000;  // samples kept for hottest()

std::string hex(u64 v) {
  char buf[24];
  std::snprintf(buf, sizeof(buf), "%llX", static_cast<unsigned long long>(v));
  return buf;
}

// "guest::__imp__fn_80012345" -> "fn_80012345": recompiled functions' host names.
// A system call that waits (a blocked thread, not work).
bool is_wait(const std::string& name) {
  if (name.rfind("Nt", 0) != 0 && name.rfind("Zw", 0) != 0) return false;
  for (const char* word : {"Wait", "Delay", "Alert", "Remove", "Yield", "Signal"})
    if (name.find(word) != std::string::npos) return true;
  return false;
}

std::FILE* open_file(const fs::path& path, const char* mode) {
#ifdef _WIN32
  std::wstring wide_mode;
  for (const char* p = mode; *p; ++p) wide_mode += static_cast<wchar_t>(*p);
  return _wfopen(path.c_str(), wide_mode.c_str());
#else
  return std::fopen(path.c_str(), mode);
#endif
}

}  // namespace

struct Monitor::State {
  Session* session = nullptr;
  Options options;
  fs::path dir;
  std::FILE* samples = nullptr;
  std::FILE* stacks_file = nullptr;
  std::FILE* symbols = nullptr;
  std::FILE* threads_file = nullptr;

  // Shared with hottest() (on other threads).
  mutable std::mutex mutex;
  struct Sample {
    u64 t_us;
    u8 thread;
    u32 stack;
  };
  std::deque<Sample> recent;
  std::vector<std::vector<u32>> stacks;  // address indices, innermost first
  std::vector<std::string> names;        // by address index: its function ("" until looked up)
  std::vector<bool> guest;               // by thread index: runs the game's code

  // The sampler thread's own.
  std::unordered_map<std::string, u32> stack_ids;  // address indices as bytes -> stack index
  std::unordered_map<u64, u32> address_ids;
  std::vector<u64> addresses;
  std::size_t symbolized = 0;
  int next_thread = 0;
  u32 samples_this_second = 0;

  // A thread index for a new thread, and its name in threads.tsv.
  u8 add_thread(u64 os_id, const std::string& name) {
    const int index = std::min(next_thread++, 255);
    name_thread(static_cast<u8>(index), os_id, name);
    return static_cast<u8>(index);
  }
  void name_thread(u8 index, u64 os_id, const std::string& name) {
    {
      std::lock_guard lock(mutex);
      if (guest.size() <= index) guest.resize(index + 1u);
      guest[index] = name.rfind("guest", 0) == 0;
    }
    if (threads_file) std::fprintf(threads_file, "%u\t%llu\t%s\n", index, static_cast<unsigned long long>(os_id), name.c_str());
  }

  // Records one sample of a stack (return addresses, innermost first).
  void record(u8 thread, const u64* frames, std::size_t depth) {
    if (depth == 0) return;
    u32 ids[kMaxDepth];
    for (std::size_t i = 0; i < depth; ++i) {
      auto [it, added] = address_ids.try_emplace(frames[i], static_cast<u32>(addresses.size()));
      if (added) {
        addresses.push_back(frames[i]);
        std::lock_guard lock(mutex);
        names.emplace_back();
      }
      ids[i] = it->second;
    }
    const std::string key(reinterpret_cast<const char*>(ids), depth * sizeof(u32));
    auto [it, added] = stack_ids.try_emplace(key, static_cast<u32>(stack_ids.size()));
    if (added) {
      const u16 d = static_cast<u16>(depth);
      if (stacks_file) {
        std::fwrite(&d, sizeof(d), 1, stacks_file);
        std::fwrite(ids, sizeof(u32), depth, stacks_file);
      }
      std::lock_guard lock(mutex);
      stacks.emplace_back(ids, ids + depth);
    }
    const u64 t = session->now_us();
    const u32 record[2] = {static_cast<u32>(t / 100), (u32{thread} << 24) | (it->second & 0xFFFFFF)};
    if (samples) std::fwrite(record, sizeof(record), 1, samples);
    ++samples_this_second;
    std::lock_guard lock(mutex);
    recent.push_back({t, thread, it->second});
    while (!recent.empty() && recent.front().t_us + kRecentUs < t) recent.pop_front();
  }
};

Monitor::Monitor() = default;

Monitor::~Monitor() { stop(); }

void Monitor::start(Session& session, const Options& options) {
  stop();
  state_ = std::make_unique<State>();
  state_->session = &session;
  state_->options = options;
  state_->dir = session.dir() / "cpu" / ("run" + std::to_string(session.run()));
  std::error_code ec;
  fs::create_directories(state_->dir, ec);
  state_->threads_file = open_file(state_->dir / "threads.tsv", "wb");
  if (options.rate_hz > 0) {
    state_->samples = open_file(state_->dir / "samples.bin", "wb");
    state_->stacks_file = open_file(state_->dir / "stacks.bin", "wb");
    state_->symbols = open_file(state_->dir / "symbols.tsv", "wb");
  }
  stopping_ = false;
  thread_ = std::thread([this] { run(); });
}

void Monitor::stop() {
  if (!thread_.joinable()) return;
  stopping_ = true;
  thread_.join();
  for (std::FILE* f : {state_->samples, state_->stacks_file, state_->symbols, state_->threads_file})
    if (f) std::fclose(f);
  state_->samples = state_->stacks_file = state_->symbols = state_->threads_file = nullptr;
}

std::vector<Monitor::Hot> Monitor::hottest(u64 from_us, u64 to_us, std::size_t count, bool inclusive) const {
  std::vector<Hot> hot;
  if (!state_) return hot;
  const State& s = *state_;
  std::lock_guard lock(s.mutex);
  std::unordered_map<std::string, u32> counts;
  u32 total = 0;
  auto name_of = [&](u32 address) -> std::string {
    return address < s.names.size() && !s.names[address].empty() ? readable_symbol(s.names[address]) : "?";
  };
  for (const State::Sample& sample : s.recent) {
    if (sample.t_us < from_us || sample.t_us >= to_us) continue;
    if (sample.thread >= s.guest.size() || !s.guest[sample.thread] || sample.stack >= s.stacks.size()) continue;
    const std::vector<u32>& stack = s.stacks[sample.stack];
    if (stack.front() < s.names.size() && is_wait(s.names[stack.front()])) continue;  // blocked, or idle
    ++total;
    if (!inclusive) {
      ++counts[name_of(stack.front())];
      continue;
    }
    std::unordered_set<std::string> seen;
    for (const u32 a : stack)
      if (seen.insert(name_of(a)).second) ++counts[name_of(a)];
  }
  if (total == 0) return hot;
  for (const auto& [name, n] : counts) {
    const double share = static_cast<double>(n) / total;
    if (inclusive && (share > 0.9 || name == "?")) continue;  // the threads' roots, on every stack
    hot.push_back({name, share});
  }
  std::sort(hot.begin(), hot.end(), [](const Hot& a, const Hot& b) { return a.share > b.share; });
  if (hot.size() > count) hot.resize(count);
  return hot;
}

#ifdef _WIN32

namespace {

struct Thread {
  HANDLE handle = nullptr;
  u8 index = 0;
  std::string name;
  u64 cpu = 0;         // CPU cycles used, at the last look (QueryThreadCycleTime: precise, unlike GetThreadTimes)
  u64 cpu_second = 0;  // ... at the start of this second
  bool busy = false;   // used the CPU since the last look: sample it
  bool own = false;    // the profiler's own (not sampled)
  u64 stack_base = 0;  // the top of its stack
  bool seen = false;
};

u64 filetime(const FILETIME& t) { return (u64{t.dwHighDateTime} << 32) | t.dwLowDateTime; }

u64 thread_cycles(HANDLE h) {
  ULONG64 cycles = 0;
  return QueryThreadCycleTime(h, &cycles) ? cycles : 0;
}

// The CPU's cycle counter, which thread cycle times count in.
u64 cycle_counter() {
#if defined(_M_X64) || defined(__x86_64__)
  return __rdtsc();
#else
  LARGE_INTEGER qpc;
  QueryPerformanceCounter(&qpc);
  return static_cast<u64>(qpc.QuadPart);  // not the same unit: CPU shares are then approximate
#endif
}

std::string thread_name(HANDLE h, DWORD id) {
  using GetDescription = HRESULT(WINAPI*)(HANDLE, PWSTR*);
  static const auto get = reinterpret_cast<GetDescription>(
      reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription")));
  std::string name;
  PWSTR wide = nullptr;
  if (get && SUCCEEDED(get(h, &wide)) && wide) {
    for (const wchar_t* p = wide; *p; ++p) name += *p < 0x80 ? static_cast<char>(*p) : '?';
    LocalFree(wide);
  }
  return name.empty() ? "thread " + std::to_string(id) : name;
}

// The top of a thread's stack, from its TEB (NT_TIB::StackBase).
u64 stack_base(HANDLE h) {
  struct BasicInformation {
    LONG exit_status;
    void* teb;
    void* client_id[2];
    ULONG_PTR affinity;
    LONG priority, base_priority;
  };
  using Query = LONG(WINAPI*)(HANDLE, int, void*, ULONG, ULONG*);
  static const auto query = reinterpret_cast<Query>(
      reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread")));
  BasicInformation info{};
  if (!query || query(h, 0, &info, sizeof(info), nullptr) != 0 || !info.teb) return 0;
  return reinterpret_cast<const NT_TIB*>(info.teb)->StackBase ? reinterpret_cast<u64>(reinterpret_cast<const NT_TIB*>(info.teb)->StackBase) : 0;
}

constexpr std::size_t kStackCopy = 64 * 1024;    // of a stopped thread's stack, from its stack pointer
constexpr std::size_t kSlack = 64 * 1024;        // past the copy, so a walk that reads past it stays in the buffer

// Walks a stack from a copy of it (`copy`: `size` bytes from context.Rsp).
std::size_t walk_copy(CONTEXT context, const u8* copy, std::size_t size, u64* frames) {
  const u64 lo = context.Rsp, hi = lo + size;
  const u64 base = reinterpret_cast<u64>(copy);
  auto rebase = [&](DWORD64& r) {
    if (r >= lo && r < hi) r = base + (r - lo);
  };
  auto rebase_all = [&] {
    for (DWORD64* r : {&context.Rsp, &context.Rbp, &context.Rbx, &context.Rsi, &context.Rdi, &context.R12,
                       &context.R13, &context.R14, &context.R15})
      rebase(*r);
  };
  rebase_all();
  std::size_t n = 0;
  while (n < kMaxDepth && context.Rip) {
    frames[n++] = context.Rip;
    if (context.Rsp < base || context.Rsp + 8 > base + size) break;
    DWORD64 image_base = 0;
    RUNTIME_FUNCTION* function = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
    if (!function) {  // leaf function: the return address is on top of the stack
      std::memcpy(&context.Rip, reinterpret_cast<const void*>(context.Rsp), 8);
      context.Rsp += 8;
      continue;
    }
    void* handler_data = nullptr;
    DWORD64 establisher = 0;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context, &handler_data, &establisher,
                     nullptr);
    rebase_all();  // registers restored from the stack hold its real addresses
  }
  return n;
}

}  // namespace

void Monitor::run() {
  os::set_thread_name("profile monitor");
  State& s = *state_;
  const HANDLE process = GetCurrentProcess();
  const DWORD self = GetCurrentThreadId();
  const DWORD pid = GetCurrentProcessId();
  const bool sampling = s.options.rate_hz > 0;
  timeBeginPeriod(1);
  std::vector<u8> copy(kStackCopy + kSlack);
  std::map<DWORD, Thread> threads;

  using clock = std::chrono::steady_clock;
  const auto period = std::chrono::microseconds(sampling ? 1'000'000 / std::clamp(s.options.rate_hz, 1, 10000) : 100'000);
  auto next_tick = clock::now();
  auto next_scan = next_tick;
  auto next_second = next_tick + std::chrono::seconds(1);
  FILETIME c, e, k, u;
  GetProcessTimes(process, &c, &e, &k, &u);
  u64 process_cpu = filetime(k) + filetime(u);
  FILETIME idle_t, kernel_t, user_t;
  GetSystemTimes(&idle_t, &kernel_t, &user_t);
  u64 sys_idle = filetime(idle_t), sys_total = filetime(kernel_t) + filetime(user_t);
  PROCESS_MEMORY_COUNTERS_EX memory{};
  GetProcessMemoryInfo(process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory));
  DWORD page_faults = memory.PageFaultCount;
  // Cycles a second (of the counter thread cycle times use), measured each second.
  u64 counter_at_second = cycle_counter();
  double cycles_per_second = 3e9;
  double busy_cycles = cycles_per_second / 500.0;
  // How long sampled threads were held stopped, this second.
  u64 stopped_ticks = 0, stopped_max = 0, copied_bytes = 0;
  LARGE_INTEGER qpc_frequency;
  QueryPerformanceFrequency(&qpc_frequency);

  // Looks the next addresses up (a few at a time: the first look into a
  // module loads its symbols).
  auto symbolize = [&](std::size_t max) {
    for (; s.symbolized < s.addresses.size() && max > 0; ++s.symbolized, --max) {
      const u64 address = s.addresses[s.symbolized];
      HostSymbol symbol = host_symbol(address);
      if (symbol.name.empty())
        symbol.name = (symbol.module.empty() ? std::string("?") : symbol.module) + "+0x" + hex(symbol.offset);
      if (s.symbols)
        std::fprintf(s.symbols, "%zu\t%llX\t%s\t%llX\t%s\n", s.symbolized, static_cast<unsigned long long>(address),
                     symbol.name.c_str(), static_cast<unsigned long long>(symbol.offset), symbol.module.c_str());
      std::lock_guard lock(s.mutex);
      s.names[s.symbolized] = std::move(symbol.name);
    }
  };

  while (!stopping_) {
    const auto now = clock::now();
    if (now >= next_scan) {
      // Threads come and go; sample the ones that used the CPU lately.
      next_scan = now + std::chrono::milliseconds(100);
      for (auto& [id, t] : threads) t.seen = false;
      const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
      THREADENTRY32 entry{};
      entry.dwSize = sizeof(entry);
      for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != pid || entry.th32ThreadID == self) continue;
        auto it = threads.find(entry.th32ThreadID);
        if (it == threads.end()) {
          const HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                                      entry.th32ThreadID);
          if (!h) continue;
          Thread t;
          t.handle = h;
          t.name = thread_name(h, entry.th32ThreadID);
          t.index = s.add_thread(entry.th32ThreadID, t.name);
          t.own = t.name.rfind("profile", 0) == 0 || t.name == "console log";
          t.stack_base = stack_base(h);
          t.cpu = t.cpu_second = thread_cycles(h);
          it = threads.emplace(entry.th32ThreadID, std::move(t)).first;
        }
        Thread& t = it->second;
        t.seen = true;
        if (std::string name = thread_name(t.handle, entry.th32ThreadID); name != t.name) {
          t.name = std::move(name);
          s.name_thread(t.index, entry.th32ThreadID, t.name);
          t.own = t.name.rfind("profile", 0) == 0 || t.name == "console log";
        }
        const u64 cpu = thread_cycles(t.handle);
        t.busy = !t.own && static_cast<double>(cpu - t.cpu) >= busy_cycles;  // 2 ms in the last 100
        t.cpu = cpu;
      }
      CloseHandle(snapshot);
      for (auto it = threads.begin(); it != threads.end();) {
        if (it->second.seen) {
          ++it;
          continue;
        }
        CloseHandle(it->second.handle);
        it = threads.erase(it);
      }
    }

    if (now >= next_second) {
      next_second += std::chrono::seconds(1);
      const u64 counter = cycle_counter();
      if (counter > counter_at_second) cycles_per_second = static_cast<double>(counter - counter_at_second);
      counter_at_second = counter;
      busy_cycles = cycles_per_second / 500.0;
      Json line;
      JsonArray per_thread;
      double others = 0;
      for (auto& [id, t] : threads) {
        const double percent = static_cast<double>(t.cpu - t.cpu_second) / cycles_per_second * 100.0;  // of one core
        t.cpu_second = t.cpu;
        if (percent >= 1.0) per_thread.add(Json().add("i", t.index).add("name", t.name).add("cpu", percent));
        else others += percent;
      }
      GetProcessTimes(process, &c, &e, &k, &u);
      const u64 pcpu = filetime(k) + filetime(u);
      GetSystemTimes(&idle_t, &kernel_t, &user_t);
      const u64 idle = filetime(idle_t), total = filetime(kernel_t) + filetime(user_t);
      const double system = total > sys_total ? 1.0 - static_cast<double>(idle - sys_idle) / static_cast<double>(total - sys_total) : -1.0;
      system_cpu_.store(system, std::memory_order_relaxed);
      GetProcessMemoryInfo(process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory));
      line.add("process_cpu", static_cast<double>(pcpu - process_cpu) / 1e5)
          .add("system_cpu", system * 100.0)
          .add("cores", static_cast<int>(std::thread::hardware_concurrency()))
          .add("working_set_mb", static_cast<double>(memory.WorkingSetSize) / 1048576.0)
          .add("private_mb", static_cast<double>(memory.PrivateUsage) / 1048576.0)
          .add("page_faults", static_cast<u64>(memory.PageFaultCount - page_faults))
          .add("samples", s.samples_this_second)
          .add("stopped_us_mean", s.samples_this_second ? static_cast<double>(stopped_ticks) * 1e6 /
                                                              static_cast<double>(qpc_frequency.QuadPart) /
                                                              s.samples_this_second
                                                        : 0.0)
          .add("stopped_us_max", static_cast<double>(stopped_max) * 1e6 / static_cast<double>(qpc_frequency.QuadPart))
          .add("copied_kb_mean", s.samples_this_second ? static_cast<double>(copied_bytes) / 1024.0 / s.samples_this_second : 0.0)
          .raw("threads", per_thread.str())
          .add("other_threads_cpu", others);
      process_cpu = pcpu;
      sys_idle = idle, sys_total = total;
      page_faults = memory.PageFaultCount;
      s.samples_this_second = 0;
      stopped_ticks = stopped_max = copied_bytes = 0;
      s.session->system_line(line);
    }

    if (sampling) {
      u64 frames[kMaxDepth];
      for (auto& [id, t] : threads) {
        if (!t.busy) continue;
        // Stopped, only its registers and stack are copied (no allocation or
        // anything else that might take a lock it holds).
        CONTEXT context{};
        context.ContextFlags = CONTEXT_FULL;
        LARGE_INTEGER stop_start, stop_end;
        QueryPerformanceCounter(&stop_start);
        if (SuspendThread(t.handle) == static_cast<DWORD>(-1)) continue;
        const bool ok = GetThreadContext(t.handle, &context);
        std::size_t size = 0;
        if (ok && t.stack_base > context.Rsp) {
          size = static_cast<std::size_t>(std::min<u64>(t.stack_base - context.Rsp, kStackCopy));
          std::memcpy(copy.data(), reinterpret_cast<const void*>(context.Rsp), size);
          copied_bytes += size;
        }
        ResumeThread(t.handle);
        QueryPerformanceCounter(&stop_end);
        const u64 stopped = static_cast<u64>(stop_end.QuadPart - stop_start.QuadPart);
        stopped_ticks += stopped;
        stopped_max = std::max(stopped_max, stopped);
        if (!ok) continue;
        s.record(t.index, frames, walk_copy(context, copy.data(), size, frames));
      }
      symbolize(32);
    }
    next_tick += period;
    if (next_tick < clock::now()) next_tick = clock::now();  // fell behind: don't catch up
    std::this_thread::sleep_until(std::min({next_tick, next_scan, next_second}));
  }
  if (sampling) symbolize(~std::size_t{0});
  timeEndPeriod(1);
  for (auto& [id, t] : threads) CloseHandle(t.handle);
}

#else

// Elsewhere: the process's CPU use and memory, and the computer's CPU use on
// Linux; no stack samples.
void Monitor::run() {
  os::set_thread_name("profile monitor");
  State& s = *state_;
  auto cpu_seconds = [] {
    rusage r{};
    getrusage(RUSAGE_SELF, &r);
    return static_cast<double>(r.ru_utime.tv_sec + r.ru_stime.tv_sec) +
           static_cast<double>(r.ru_utime.tv_usec + r.ru_stime.tv_usec) * 1e-6;
  };
  // /proc/stat's first line: the computer's busy and total time.
  auto system_times = [](u64* busy, u64* total) {
    std::ifstream f("/proc/stat");
    std::string cpu;
    u64 v[8] = {};
    if (!(f >> cpu) || cpu != "cpu") return false;
    for (u64& x : v) f >> x;
    *total = 0;
    for (u64 x : v) *total += x;
    *busy = *total - v[3] - v[4];  // idle, iowait
    return true;
  };
  double last_cpu = cpu_seconds();
  u64 last_busy = 0, last_total = 0;
  const bool have_system = system_times(&last_busy, &last_total);
  auto next = std::chrono::steady_clock::now();
  while (!stopping_) {
    next += std::chrono::seconds(1);
    while (!stopping_ && std::chrono::steady_clock::now() < next) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if (stopping_) break;
    const double cpu = cpu_seconds();
    rusage r{};
    getrusage(RUSAGE_SELF, &r);
    Json line;
    line.add("process_cpu", (cpu - last_cpu) * 100.0).add("cores", static_cast<int>(std::thread::hardware_concurrency()));
#ifdef __APPLE__
    line.add("max_rss_mb", static_cast<double>(r.ru_maxrss) / 1048576.0);  // bytes
#else
    line.add("max_rss_mb", static_cast<double>(r.ru_maxrss) / 1024.0);  // KiB
#endif
    u64 busy = 0, total = 0;
    if (have_system && system_times(&busy, &total) && total > last_total) {
      const double system = static_cast<double>(busy - last_busy) / static_cast<double>(total - last_total);
      system_cpu_.store(system, std::memory_order_relaxed);
      line.add("system_cpu", system * 100.0);
      last_busy = busy, last_total = total;
    }
    last_cpu = cpu;
    s.session->system_line(line);
  }
}

#endif

}  // namespace profile
