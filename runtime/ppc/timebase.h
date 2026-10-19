// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Gekko's time base and the GameCube's clock, in one place for the CPU
// runtime, the hardware and boot.
#pragma once

#include <cstdint>
#include <ctime>

namespace ppc {

// 40.5 MHz: the time base ticks at the bus clock (162 MHz) / 4.
inline constexpr std::uint64_t kTimebaseHz = 40500000;

// Ticks <-> nanoseconds (1e9 / 40.5e6 = 2000 / 81), without overflowing for
// any u64 input (ns * kTimebaseHz would after about 7.6 minutes).
constexpr std::uint64_t ns_to_ticks(std::uint64_t ns) { return (ns / 2000) * 81 + (ns % 2000) * 81 / 2000; }
constexpr std::uint64_t ticks_to_ns(std::uint64_t ticks) { return (ticks / 81) * 2000 + (ticks % 81) * 2000 / 81; }
// Rounded up, so that ns_to_ticks(ticks_to_ns_up(t)) == t.
constexpr std::uint64_t ticks_to_ns_up(std::uint64_t ticks) {
  return (ticks / 81) * 2000 + ((ticks % 81) * 2000 + 80) / 81;
}

// Seconds since 2000-01-01 00:00 in local time: the GameCube's epoch, for
// the time base at boot and the real-time clock.
inline std::uint64_t local_seconds_since_2000() {
  const std::time_t now = std::time(nullptr);
  std::tm local{}, utc{};
#ifdef _WIN32
  localtime_s(&local, &now);
  gmtime_s(&utc, &now);
#else
  localtime_r(&now, &local);
  gmtime_r(&now, &utc);
#endif
  local.tm_isdst = 0;
  utc.tm_isdst = 0;
  const auto offset = static_cast<long long>(std::difftime(std::mktime(&local), std::mktime(&utc)));
  constexpr long long kUnixTo2000 = 946684800;
  return static_cast<std::uint64_t>(static_cast<long long>(now) + offset - kUnixTo2000);
}

}  // namespace ppc
