// SPDX-License-Identifier: MIT
#pragma once
#include "json.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>
namespace ac {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
inline double seconds(Clock::time_point t) {
  return std::chrono::duration<double>(Clock::now() - t).count();
}
inline std::string read_file(const std::string &p, size_t cap = 65536) {
  std::ifstream f(p.c_str(), std::ios::binary);
  std::string out;
  char b[4096];
  while (f && out.size() < cap) {
    f.read(b, std::min(sizeof(b), cap - out.size()));
    out.append(b, size_t(f.gcount()));
  }
  return out;
}
inline void close_fd(int &fd) {
  if (fd >= 0) {
    close(fd);
    fd = -1;
  }
}
struct Format {
  std::string sample = "S16_LE";
  unsigned rate = 48000, channels = 2;
  unsigned bytes() const { return sample == "S32_LE" ? 4 : 2; }
  Json json() const { return {{"sample_format", sample}, {"rate", rate}, {"channels", channels}}; }
};
struct Config {
  std::string backend = "alsa", device = "", transport = "rtp", destination = "127.0.0.1",
              profile = "low-latency";
  Format fmt;
  unsigned port = 5004, packet_ms = 10;
  bool http_lan = false, autostart = false, confirmed = false;
  Json json() const {
    return {{"backend", backend},
            {"device", device},
            {"transport", transport},
            {"destination", destination},
            {"destination_port", port},
            {"sample_format", fmt.sample},
            {"rate", fmt.rate},
            {"channels", fmt.channels},
            {"packet_ms", packet_ms},
            {"profile", profile},
            {"http_lan", http_lan},
            {"autostart", autostart},
            {"playback_confirmed", confirmed}};
  }
  static Config parse(const Json &, const Config &, bool allow_test);
};
struct Block {
  std::vector<uint8_t> pcm;
  uint64_t frame = 0;
  Clock::time_point captured;
  bool discontinuity = false;
};
class Ring {
  std::mutex mu_;
  std::condition_variable ready_;
  std::deque<Block> q_;
  size_t limit_;
  uint64_t drops_ = 0;

public:
  explicit Ring(size_t n = 8) : limit_(n) {}
  void push(Block b) {
    std::lock_guard<std::mutex> l(mu_);
    if (q_.size() >= limit_) {
      q_.pop_front();
      ++drops_;
      b.discontinuity = true;
    }
    q_.push_back(std::move(b));
    ready_.notify_one();
  }
  bool pop(Block &b) {
    std::lock_guard<std::mutex> l(mu_);
    if (q_.empty())
      return false;
    b = std::move(q_.front());
    q_.pop_front();
    return true;
  }
  void clear() {
    std::lock_guard<std::mutex> l(mu_);
    q_.clear();
  }
  void wait(unsigned milliseconds) {
    std::unique_lock<std::mutex> l(mu_);
    ready_.wait_for(l, std::chrono::milliseconds(milliseconds), [&] { return !q_.empty(); });
  }
  uint64_t drops() {
    std::lock_guard<std::mutex> l(mu_);
    return drops_;
  }
};
inline std::vector<double> rms(const std::vector<uint8_t> &data, const Format &f,
                               std::vector<double> &peaks) {
  std::vector<double> s(f.channels, 0);
  peaks.assign(f.channels, 0);
  size_t n = data.size() / (f.bytes() * f.channels);
  for (size_t i = 0; i < n; ++i)
    for (unsigned c = 0; c < f.channels; ++c) {
      const uint8_t *p = &data[(i * f.channels + c) * f.bytes()];
      double x;
      if (f.bytes() == 2) {
        int16_t v = int16_t(uint16_t(p[0]) | (uint16_t(p[1]) << 8));
        x = v / 32768.0;
      } else {
        uint32_t u = uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
                     (uint32_t(p[3]) << 24);
        x = int32_t(u) / 2147483648.0;
      }
      s[c] += x * x;
      peaks[c] = std::max(peaks[c], std::abs(x));
    }
  for (double &x : s)
    x = n ? std::sqrt(x / n) : 0;
  return s;
}
std::string random_token(size_t bytes = 32);
void atomic_file(const std::string &, const std::string &);
Json discover();
Json system_info();
} // namespace ac
