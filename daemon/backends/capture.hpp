// SPDX-License-Identifier: MIT
#pragma once
#include "../common.hpp"
#include <memory>
namespace ac {
class Capture {
public:
  virtual ~Capture() {};
  virtual void open(const Config &) = 0;
  // Return complete interleaved PCM frames only. Empty on timeout. Discontinuity on xrun.
  virtual std::vector<uint8_t> read(unsigned frames, bool &discontinuity) = 0;
  virtual unsigned long delay_frames() const = 0;
};
std::unique_ptr<Capture> make_capture(const std::string &backend, bool allow_test);
Json probe_device(const std::string &device);
} // namespace ac
