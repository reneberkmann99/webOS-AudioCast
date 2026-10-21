// SPDX-License-Identifier: MIT
#include "engine.hpp"
#include "backends/capture.hpp"
#include "network/rtp.hpp"
#include <fcntl.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
namespace ac {
Engine::Engine(std::string dir, bool allow) : dir_(std::move(dir)), allow_test_(allow), ring(8) {
  if (mkdir(dir_.c_str(), 0700) < 0 && errno != EEXIST)
    throw std::runtime_error("cannot create state directory");
  struct stat st{};
  if (lstat(dir_.c_str(), &st) < 0 || !S_ISDIR(st.st_mode) || st.st_uid != geteuid())
    throw std::runtime_error("state directory must be owned by this user and not be a symlink");
  if (chmod(dir_.c_str(), 0700) < 0)
    throw std::runtime_error("cannot secure state directory");
  int lock = open((dir_ + "/init.lock").c_str(), O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (lock < 0 || flock(lock, LOCK_EX) < 0)
    throw std::runtime_error("cannot lock initialization");
  try {
    std::string raw = read_file(dir_ + "/credentials.json", 4096);
    if (raw.empty()) {
      control_token = random_token();
      stream_token = random_token();
      atomic_file(dir_ + "/credentials.json",
                  Json({{"control_token", control_token}, {"stream_token", stream_token}}).dump() +
                      "\n");
    } else {
      Json j = Json::parse(raw);
      control_token = j.at("control_token").get<std::string>();
      stream_token = j.at("stream_token").get<std::string>();
      if (control_token.size() != 64 || stream_token.size() != 64 ||
          control_token.find_first_not_of("0123456789abcdef") != std::string::npos ||
          stream_token.find_first_not_of("0123456789abcdef") != std::string::npos)
        throw std::runtime_error("invalid credentials file");
    }
    raw = read_file(dir_ + "/config.json", 8192);
    if (!raw.empty())
      config_ = Config::parse(Json::parse(raw), config_, allow_test_);
    else
      persist();
  } catch (...) {
    close(lock);
    throw;
  }
  close(lock);
  stats_ = {{"state", "idle"},
            {"signal", "unknown"},
            {"last_error", ""},
            {"http_clients", 0},
            {"rtp_packets", 0},
            {"rtp_errors", 0},
            {"capture_overruns", 0},
            {"throughput_bps", 0.0},
            {"cpu_percent", 0.0},
            {"rss_kb", 0},
            {"capture_to_network_ms", nullptr},
            {"alsa_pending_ms", nullptr},
            {"end_to_end_ms", nullptr},
            {"levels", {{"rms", Json::array({0.0, 0.0})}, {"peak", Json::array({0.0, 0.0})}}},
            {"audio_output", "unknown"},
            {"silence_seconds", 0.0}};
}
Engine::~Engine() {
  shutdown();
}
void Engine::launch() {
  capture_ = std::thread(&Engine::capture_loop, this);
  network_ = std::thread(&Engine::network_loop, this);
  Config c = config();
  if (c.autostart && c.confirmed) {
    try {
      start();
    } catch (const std::exception &e) {
      log(e.what());
    }
  }
}
void Engine::shutdown() {
  quit_ = true;
  intent_ = false;
  if (capture_.joinable())
    capture_.join();
  if (network_.joinable())
    network_.join();
}
Config Engine::config() const {
  std::lock_guard<std::mutex> l(mu_);
  return config_;
}
void Engine::persist() {
  atomic_file(dir_ + "/config.json", config_.json().dump(2) + "\n");
}
void Engine::stat(const std::string &k, const Json &v) {
  std::lock_guard<std::mutex> l(mu_);
  stats_[k] = v;
}
void Engine::add(const std::string &k, uint64_t n) {
  std::lock_guard<std::mutex> l(mu_);
  stats_[k] = stats_.value(k, uint64_t(0)) + n;
}
void Engine::log(const std::string &s) {
  std::lock_guard<std::mutex> l(mu_);
  std::string line = std::to_string(uint64_t(seconds(born_))) + "s " + s.substr(0, 500);
  logs_.push_back(line);
  if (logs_.size() > 150)
    logs_.pop_front();
}
Json Engine::logs() {
  std::lock_guard<std::mutex> l(mu_);
  return {{"lines", logs_}};
}
Json Engine::status() {
  std::lock_guard<std::mutex> l(mu_);
  Json j = stats_;
  j["streaming"] = streaming();
  j["test_active"] = bool(testing_ && intent_);
  j["backend"] = config_.backend;
  j["device"] = config_.device;
  j["source_verified"] = config_.confirmed;
  j["format"] = config_.fmt.json();
  j["destination"] = config_.destination + ":" + std::to_string(config_.port);
  j["transport"] = config_.transport;
  j["rtp_receivers"] = nullptr;
  j["queue_drops"] = ring.drops();
  j["uptime_seconds"] = seconds(born_);
  j["config"] = config_.json();
  j["hardware_qualified"] = false;
  int64_t last = last_capture_ms_;
  j["capture_sample_age_ms"] = last > 0
                                   ? Json(std::chrono::duration_cast<std::chrono::milliseconds>(
                                              Clock::now().time_since_epoch())
                                              .count() -
                                          last)
                                   : Json(nullptr);
  return j;
}
void Engine::update_config(const Json &j) {
  if (intent_ || active_)
    throw std::runtime_error("stop capture before changing configuration");
  std::lock_guard<std::mutex> l(mu_);
  Config c = Config::parse(j, config_, allow_test_);
  Config old = config_;
  config_ = c;
  try {
    persist();
  } catch (...) {
    config_ = old;
    throw;
  }
  ++generation_;
}
void Engine::start(bool test) {
  if (intent_) {
    if (test != testing_)
      throw std::runtime_error("stop current capture first");
    return;
  }
  Config c = config();
  if (c.backend == "alsa" && c.device.empty())
    throw std::runtime_error(
        "select explicit playback capture device first; no microphone fallback");
  if (!test && !c.confirmed)
    throw std::runtime_error("confirm selected device captures known TV playback before streaming");
  testing_ = test;
  last_capture_ms_ = 0;
  paused_ = false;
  ring.clear();
  ++generation_;
  stat("last_error", "");
  stat("signal", "unknown");
  stat("state", "starting");
  intent_ = true;
  log(test ? "Capture-only test started (no network/audio storage)" : "Streaming requested");
}
void Engine::stop() {
  intent_ = false;
  testing_ = false;
  paused_ = false;
  ring.clear();
  ++generation_;
  stat("state", "idle");
  stat("levels", {{"rms", Json::array({0.0, 0.0})}, {"peak", Json::array({0.0, 0.0})}});
  stat("signal", "unknown");
  log("Streaming stopped");
}
void Engine::suspend() {
  paused_ = true;
  ++generation_;
  ring.clear();
  stat("state", "paused");
  log("Capture suspended; device released on next bounded read");
}
void Engine::resume() {
  paused_ = false;
  ++generation_;
  stat("state", intent_ ? "starting" : "idle");
  log("Capture resume requested");
}
Json Engine::route(const std::string &m, const std::string &p, const Json &b) {
  if (m == "GET" && p == "/v1/status")
    return status();
  if (m == "GET" && p == "/v1/devices")
    return discover();
  if (m == "GET" && p == "/v1/config")
    return config().json();
  if (m == "GET" && p == "/v1/logs")
    return logs();
  if (m == "GET" && p == "/v1/diagnostics")
    return {{"status", status()},
            {"devices", discover()},
            {"config", config().json()},
            {"system", system_info()},
            {"privacy", "No captured audio or authentication tokens included. Inspect "
                        "model/hostname before sharing."}};
  if (m == "POST" && p == "/v1/config") {
    update_config(b);
    return config().json();
  }
  if (m == "POST" && p == "/v1/start") {
    start();
    return {{"accepted", true}};
  }
  if (m == "POST" && p == "/v1/stop") {
    stop();
    return {{"stopped", true}};
  }
  if (m == "POST" && p == "/v1/test-capture") {
    start(true);
    return {
        {"accepted", true},
        {"duration_seconds", 5},
        {"note",
         "Correlate meters with known TV playback; signal alone is not proof of playback source."}};
  }
  if (m == "POST" && p == "/v1/suspend") {
    suspend();
    return {{"paused", true}};
  }
  if (m == "POST" && p == "/v1/resume") {
    resume();
    return {{"accepted", true}};
  }
  if (m == "POST" && p == "/v1/test-connection") {
    Config c = config();
    if (c.transport == "http")
      return {{"reachable", nullptr},
              {"delivery_verified", false},
              {"message", "HTTP stream listener is on port7070; LAN access requires http_lan. A "
                          "receiver must authenticate and consume samples to prove delivery."}};
    Rtp probe;
    probe.open(c);
    return {{"reachable", nullptr},
            {"delivery_verified", false},
            {"message",
             "LAN destination resolved and UDP socket created. UDP has no receiver handshake; no "
             "audio-delivery claim. Run the reference receiver and verify its real levels."}};
  }
  throw std::runtime_error("unknown method or endpoint");
}
void Engine::capture_loop() {
  unsigned backoff = 100;
  uint64_t frame = 0;
  unsigned frame_gen = ~0U;
  auto test_start = Clock::now();
  while (!quit_) {
    if (!intent_ || paused_) {
      active_ = false;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      continue;
    }
    active_ = true;
    Config c = config();
    unsigned gen = generation_;
    if (gen != frame_gen) {
      frame_gen = gen;
      frame = 0;
      test_start = Clock::now();
    }
    if (testing_ && seconds(test_start) >= 5) {
      stop();
      active_ = false;
      continue;
    }
    auto last_signal = Clock::now();
    auto last_read = Clock::now();
    bool pending_disc = true;
    try {
      auto src = make_capture(c.backend, allow_test_);
      src->open(c);
      stat("state", "capturing");
      log("Capture opened: " + c.backend + " " + c.device);
      while (!quit_ && intent_ && !paused_ && gen == generation_) {
        if (testing_ && seconds(test_start) >= 5) {
          stop();
          break;
        }
        bool disc = false;
        auto data = src->read(std::max(1U, c.fmt.rate * c.packet_ms / 1000), disc);
        if (disc) {
          pending_disc = true;
          add("capture_overruns", 1);
        }
        if (data.empty()) {
          if (seconds(last_read) > 3)
            throw std::runtime_error("capture produced no frames for 3 seconds");
          continue;
        }
        if (!intent_ || paused_ || gen != generation_)
          break;
        backoff = 100;
        last_read = Clock::now();
        auto captured = Clock::now();
        last_capture_ms_ =
            std::chrono::duration_cast<std::chrono::milliseconds>(captured.time_since_epoch())
                .count();
        std::vector<double> peak;
        auto levels = rms(data, c.fmt, peak);
        bool signal = *std::max_element(peak.begin(), peak.end()) > 0.0001;
        if (signal)
          last_signal = captured;
        stat("levels", {{"rms", levels}, {"peak", peak}});
        stat("signal", signal ? "detected" : "silence");
        stat("silence_seconds", seconds(last_signal));
        stat("alsa_pending_ms", 1000.0 * src->delay_frames() / c.fmt.rate);
        size_t frames = data.size() / (c.fmt.channels * c.fmt.bytes());
        if (!testing_) {
          Block b;
          b.pcm = std::move(data);
          b.frame = frame;
          b.captured = captured;
          b.discontinuity = pending_disc;
          ring.push(std::move(b));
        }
        frame += frames;
        pending_disc = false;
      }
    } catch (const std::exception &e) {
      stat("last_error", e.what());
      stat("state", "recovering");
      stat("levels", {{"rms", Json::array({0.0, 0.0})}, {"peak", Json::array({0.0, 0.0})}});
      log(std::string("Capture failure: ") + e.what());
      for (unsigned i = 0; i < backoff / 20 && !quit_ && intent_ && !paused_ &&
                           !(testing_ && seconds(test_start) >= 5);
           ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      backoff = std::min(10000U, backoff * 2);
      if (testing_ && seconds(test_start) >= 5)
        stop();
    }
    active_ = false;
  }
}
} // namespace ac
