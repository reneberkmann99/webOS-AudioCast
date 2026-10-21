// SPDX-License-Identifier: MIT
#pragma once
#include "common.hpp"
namespace ac {
class Engine {
  mutable std::mutex mu_;
  Config config_;
  Json stats_;
  std::deque<std::string> logs_;
  std::string dir_;
  bool allow_test_;
  Clock::time_point born_ = Clock::now();
  std::thread capture_, network_;
  std::atomic<bool> quit_{false}, intent_{false}, testing_{false}, paused_{false}, active_{false};
  std::atomic<unsigned> generation_{0};
  std::atomic<int64_t> last_capture_ms_{0};
  std::atomic<int64_t> dile_deadline_ms_{0};
  void capture_loop();
  void network_loop();
  void persist();

public:
  Ring ring;
  std::string control_token, stream_token;
  Engine(std::string dir, bool allow_test);
  ~Engine();
  void launch();
  void shutdown();
  Config config() const;
  Json status();
  Json logs();
  void log(const std::string &);
  void stat(const std::string &, const Json &);
  void add(const std::string &, uint64_t);
  void update_config(const Json &);
  void start(bool test = false);
  void stop();
  void suspend();
  void resume();
  bool streaming() const { return intent_ && !testing_ && !paused_; }
  bool quitting() const { return quit_; }
  unsigned generation() const { return generation_; }
  Json route(const std::string &method, const std::string &path, const Json &body);
};
} // namespace ac
