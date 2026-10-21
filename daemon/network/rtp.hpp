// SPDX-License-Identifier: MIT
#pragma once
#include "../common.hpp"
#include <netdb.h>
namespace ac {
class Rtp {
  int fd_ = -1;
  sockaddr_storage dest_{};
  socklen_t len_ = 0;
  uint16_t seq_ = 0;
  uint32_t ssrc_ = 0, base_ = 0;
  unsigned channels_ = 2;
  uint64_t expected_ = 0;
  bool first_ = true;
  sockaddr_storage rtcp_dest_{};
  unsigned rate_ = 48000;
  uint32_t packet_count_ = 0, octet_count_ = 0;
  uint64_t anchor_frame_ = 0;
  Clock::time_point anchor_ = Clock::now(), rtcp_at_ = Clock::now();
  double rtcp_interval_ = 0;
  std::vector<uint8_t> rtcp_packet(bool bye = false) const;
  void send_rtcp(uint64_t &bytes, bool bye = false);

public:
  ~Rtp() {
    if (packet_count_) {
      uint64_t ignored = 0;
      send_rtcp(ignored, true);
    }
    close_fd(fd_);
  }
  void open(const Config &);
  std::vector<std::vector<uint8_t>> packetize(const Block &);
  std::pair<uint64_t, uint64_t> send(const Block &, uint64_t &bytes);
};
std::string sdp(const Config &);
} // namespace ac
