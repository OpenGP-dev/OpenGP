// SPDX-License-Identifier: GPL-3.0-or-later

#include "discord/presence.h"

#include <chrono>
#include <cstdio>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace discord {

namespace {

long process_id() {
#ifdef _WIN32
  return static_cast<long>(GetCurrentProcessId());
#else
  return static_cast<long>(getpid());
#endif
}

// A JSON string literal.
std::string quote(std::string_view s) {
  std::string out = "\"";
  for (const char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
          out += buf;
        } else {
          out += c;  // UTF-8 as it is
        }
    }
  }
  return out + "\"";
}

// `"key":"value"` after a comma when not first; nothing for an empty value.
void field(std::string& out, bool& first, std::string_view key, std::string_view value) {
  if (value.empty()) return;
  if (!first) out += ',';
  first = false;
  out += quote(key) + ':' + quote(value);
}

// What Discord says in an ERROR event or a CLOSE message, if anything.
std::string error_message(const std::string& json) {
  const auto at = json.find("\"message\":\"");
  if (at == std::string::npos) return json;
  const auto start = at + 11;
  return json.substr(start, json.find('"', start) - start);
}

}  // namespace

std::string handshake_json(std::string_view application_id) {
  return "{\"v\":1,\"client_id\":" + quote(application_id) + "}";
}

std::string set_activity_json(const std::optional<Activity>& activity, long pid, std::string_view nonce) {
  std::string out = "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" + std::to_string(pid);
  if (activity) {
    const Activity& a = *activity;
    out += ",\"activity\":{";
    bool first = true;
    field(out, first, "details", a.details);
    field(out, first, "state", a.state);
    if (a.start) {
      if (!first) out += ',';
      first = false;
      out += "\"timestamps\":{\"start\":" + std::to_string(a.start) + "}";
    }
    std::string assets;
    bool first_asset = true;
    field(assets, first_asset, "large_image", a.large_image);
    field(assets, first_asset, "large_text", a.large_text);
    field(assets, first_asset, "small_image", a.small_image);
    field(assets, first_asset, "small_text", a.small_text);
    if (!assets.empty()) {
      if (!first) out += ',';
      first = false;
      out += "\"assets\":{" + assets + "}";
    }
    out += "}";
  }
  return out + "},\"nonce\":" + quote(nonce) + "}";
}

Presence::Presence(std::string application_id, Options options)
    : id_(std::move(application_id)), options_(std::move(options)) {
  thread_ = std::thread([this] { run(); });
}

Presence::~Presence() {
  {
    std::lock_guard lock(mutex_);
    stop_ = true;
  }
  wake_.notify_all();
  thread_.join();
}

void Presence::set(std::optional<Activity> activity) {
  {
    std::lock_guard lock(mutex_);
    if (wanted_ == activity) return;
    wanted_ = std::move(activity);
  }
  wake_.notify_all();
}

bool Presence::handshake(Transport& t) {
  t.write({Opcode::Handshake, handshake_json(id_)});
  // Discord answers with a READY event, or closes with the reason.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (t.connected() && std::chrono::steady_clock::now() < deadline) {
    const auto message = t.read(100);
    if (!message) continue;
    if (message->op == Opcode::Close) {
      if (options_.log) std::fprintf(stderr, "[discord] %s\n", error_message(message->json).c_str());
      return false;
    }
    if (message->op == Opcode::Frame && message->json.find("\"evt\":\"READY\"") != std::string::npos) {
      if (options_.log) std::fprintf(stderr, "[discord] connected\n");
      return true;
    }
  }
  return false;
}

void Presence::run() {
  using clock = std::chrono::steady_clock;
  const auto retry = std::chrono::milliseconds(options_.retry_ms);
  const auto interval = std::chrono::milliseconds(options_.min_interval_ms);
  std::unique_ptr<Transport> t;
  auto next_attempt = clock::now();
  clock::time_point last_send{};
  std::optional<std::optional<Activity>> shown;  // what Discord shows, once set
  unsigned nonce = 0;
  std::string last_error;

  std::unique_lock lock(mutex_);
  while (!stop_) {
    const std::optional<Activity> wanted = wanted_;
    lock.unlock();
    auto now = clock::now();
    if (!t && now >= next_attempt) {
      next_attempt = now + retry;
      t = options_.connect();
      if (t && !handshake(*t)) t.reset();
      shown.reset();
      now = clock::now();
    }
    auto wait = std::chrono::duration_cast<clock::duration>(std::chrono::milliseconds(500));
    if (t) {
      // Discord's answers (an error, say), pings, or its closing.
      while (t->connected()) {
        const auto message = t->read(0);
        if (!message) break;
        if (message->op == Opcode::Ping) {
          t->write({Opcode::Pong, message->json});
        } else if (message->op == Opcode::Close) {
          t.reset();
          break;
        } else if (message->json.find("\"evt\":\"ERROR\"") != std::string::npos) {
          const std::string error = error_message(message->json);
          if (options_.log && error != last_error) std::fprintf(stderr, "[discord] %s\n", error.c_str());
          last_error = error;
        }
      }
      const bool changed = shown ? *shown != wanted : wanted.has_value();
      if (t && t->connected() && changed) {
        if (now - last_send >= interval) {
          t->write({Opcode::Frame, set_activity_json(wanted, process_id(), std::to_string(++nonce))});
          shown = wanted;
          last_send = now;
        } else {
          wait = std::min(wait, interval - (now - last_send));
        }
      }
      if (t && !t->connected()) t.reset();
      if (!t) {
        if (options_.log) std::fprintf(stderr, "[discord] disconnected\n");
        next_attempt = now + retry;
      }
    }
    connected_ = t != nullptr;
    if (!t) wait = std::min(wait, std::max<clock::duration>(next_attempt - now, clock::duration::zero()));
    lock.lock();
    wake_.wait_for(lock, wait, [&] { return stop_ || wanted_ != wanted; });
  }
}

}  // namespace discord
