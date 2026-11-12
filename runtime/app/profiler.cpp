// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/profiler.h"

#include <algorithm>
#include <cstddef>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#else
#include <fstream>
#endif

namespace app {
using gekko::u64;

namespace {
u64 delta(u64 now, u64 before) { return now >= before ? now - before : 0; }
float ms(u64 ns) { return static_cast<float>(static_cast<double>(ns) * 1e-6); }

bool cpu_counters(u64& total, u64& idle) {
#ifdef _WIN32
  FILETIME i{}, k{}, u{};
  if (!GetSystemTimes(&i, &k, &u)) return false;
  auto value = [](const FILETIME& t) { return (u64{t.dwHighDateTime} << 32) | t.dwLowDateTime; };
  idle = value(i);
  total = value(k) + value(u);  // kernel includes idle
#elif defined(__APPLE__)
  host_cpu_load_info_data_t ticks{};
  mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
  const mach_port_t host = mach_host_self();
  const kern_return_t result = host_statistics(host, HOST_CPU_LOAD_INFO,
      reinterpret_cast<host_info_t>(&ticks), &count);
  mach_port_deallocate(mach_task_self(), host);
  if (result != KERN_SUCCESS) return false;
  total = 0;
  for (const auto tick : ticks.cpu_ticks) total += tick;
  idle = ticks.cpu_ticks[CPU_STATE_IDLE];
#else
  std::ifstream file("/proc/stat");
  std::string name;
  u64 user = 0, nice = 0, system = 0, waiting = 0, irq = 0, softirq = 0, steal = 0;
  if (!(file >> name >> user >> nice >> system >> idle >> waiting >> irq >> softirq >> steal) || name != "cpu")
    return false;
  idle += waiting;
  total = user + nice + system + idle + irq + softirq + steal;
#endif
  return true;
}
}  // namespace

void Profiler::sample_system_cpu() {
  u64 total = 0, idle = 0;
  if (!cpu_counters(total, idle)) {
    live_.system_cpu = -1;
    have_cpu_ = false;
    return;
  }
  if (have_cpu_ && total > cpu_total_ && idle >= cpu_idle_) {
    const double busy = 1.0 - static_cast<double>(idle - cpu_idle_) / static_cast<double>(total - cpu_total_);
    live_.system_cpu = static_cast<float>(std::clamp(busy * 100.0, 0.0, 100.0));
  }
  cpu_total_ = total;
  cpu_idle_ = idle;
  have_cpu_ = true;
}

void Profiler::on_frame(const FrameInfo& f) {
  auto& l = live_;
  l.render_ms = ms(f.render_ns);
  l.audio_ms = static_cast<float>(f.audio_ms);
  l.audio_underruns = f.audio_underruns;
  l.textures = f.textures;
  l.pipelines = f.pipelines;
  l.texture_mb = static_cast<double>(f.texture_bytes) / (1024.0 * 1024.0);
  l.scene = f.scene;
  l.course = f.course;

  // A paused/menu interval, first frame, or reset cannot be a gameplay hitch.
  const bool steady = have_last_ && f.active && last_.active &&
                      f.finished_ns > last_.finished_ns && f.present_ns > last_.present_ns;
  if (steady) {
    const u64 guest = f.finished_ns - last_.finished_ns;
    const u64 present = f.present_ns - last_.present_ns;
    const u64 idle = std::min(guest, delta(f.idle_ns, last_.idle_ns));
    const u64 wait = std::min(guest - idle, delta(f.wait_ns, last_.wait_ns));
    l.guest_ms = ms(guest);
    l.busy_ms = ms(guest - idle - wait);
    l.present_ms = ms(present);

    float expected = 16.683f;
    if (l.graph.size() >= 30) {
      auto sorted = l.graph;
      const auto middle = sorted.begin() + static_cast<std::ptrdiff_t>(sorted.size() / 2);
      std::nth_element(sorted.begin(), middle, sorted.end());
      expected = *middle;
    }
    const float slow = std::max(expected * 1.5f, expected + 8.0f);
    if (l.guest_ms > slow || l.present_ms > slow) ++l.hitches;
    if (f.retrace > last_.retrace) l.drops += f.retrace - last_.retrace - 1;
    l.graph.push_back(l.guest_ms);
    if (l.graph.size() > 120) l.graph.erase(l.graph.begin());
    auto sorted = l.graph;
    const auto index = static_cast<std::size_t>(0.99 * static_cast<double>(sorted.size() - 1));
    std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(index), sorted.end());
    l.p99_ms = sorted[index];
    present_average_ns_ = present_average_ns_ > 0 ? present_average_ns_ * 0.9 + static_cast<double>(present) * 0.1
                                                 : static_cast<double>(present);
    l.fps = static_cast<float>(1e9 / present_average_ns_);
  }
  last_ = f;
  have_last_ = true;
}

}  // namespace app
