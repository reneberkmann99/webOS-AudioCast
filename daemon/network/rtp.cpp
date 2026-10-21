// SPDX-License-Identifier: MIT
#include "rtp.hpp"
#include <arpa/inet.h>
#include <fcntl.h>
#include <sstream>
namespace ac {
static bool local_address(const sockaddr *a) {
  if (a->sa_family == AF_INET) {
    uint32_t x = ntohl(reinterpret_cast<const sockaddr_in *>(a)->sin_addr.s_addr);
    if ((x & 255) == 255)
      return false;
    return (x >> 24) == 10 || (x >> 24) == 127 || (x >> 20) == 0xac1 || (x >> 16) == 0xc0a8 ||
           (x >> 16) == 0xa9fe;
  }
  if (a->sa_family == AF_INET6) {
    const auto *p = reinterpret_cast<const sockaddr_in6 *>(a)->sin6_addr.s6_addr;
    return IN6_IS_ADDR_LOOPBACK(&reinterpret_cast<const sockaddr_in6 *>(a)->sin6_addr) ||
           (p[0] & 0xfe) == 0xfc || (p[0] == 0xfe && (p[1] & 0xc0) == 0x80);
  }
  return false;
}
void Rtp::open(const Config &c) {
  channels_ = c.fmt.channels;
  rate_ = c.fmt.rate;
  addrinfo hints{};
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_family = AF_UNSPEC;
  addrinfo *result = nullptr;
  int r = getaddrinfo(c.destination.c_str(), std::to_string(c.port).c_str(), &hints, &result);
  if (r)
    throw std::runtime_error(std::string("destination resolution: ") + gai_strerror(r));
  bool ok = false;
  for (addrinfo *a = result; a; a = a->ai_next) {
    if (!local_address(a->ai_addr))
      continue;
    int fd = socket(a->ai_family, SOCK_DGRAM, 0);
    if (fd < 0)
      continue;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    int b = 16384;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &b, sizeof(b));
    close_fd(fd_);
    fd_ = fd;
    memcpy(&dest_, a->ai_addr, a->ai_addrlen);
    len_ = socklen_t(a->ai_addrlen);
    ok = true;
    break;
  }
  freeaddrinfo(result);
  if (!ok)
    throw std::runtime_error(
        "destination must resolve to a LAN, link-local, or loopback unicast address");
  std::string seed = random_token(10);
  seq_ = uint16_t(std::stoul(seed.substr(0, 4), nullptr, 16));
  ssrc_ = uint32_t(std::stoul(seed.substr(4, 8), nullptr, 16));
  base_ = uint32_t(std::stoul(seed.substr(12, 8), nullptr, 16));
  first_ = true;
  rtcp_dest_ = dest_;
  unsigned rtcp_port = c.port == 65535 ? 65534 : c.port + 1;
  if (rtcp_dest_.ss_family == AF_INET)
    reinterpret_cast<sockaddr_in *>(&rtcp_dest_)->sin_port = htons(uint16_t(rtcp_port));
  else
    reinterpret_cast<sockaddr_in6 *>(&rtcp_dest_)->sin6_port = htons(uint16_t(rtcp_port));
  packet_count_ = octet_count_ = 0;
  rtcp_interval_ = 0;
  rtcp_at_ = Clock::now();
}
std::vector<std::vector<uint8_t>> Rtp::packetize(const Block &b) {
  std::vector<std::vector<uint8_t>> out;
  const size_t align = 2 * channels_;
  if (b.pcm.size() % align)
    throw std::runtime_error("unaligned L16 block");
  const size_t max_frames = 1188 / align;
  size_t frames = b.pcm.size() / align;
  for (size_t off = 0; off < frames;) {
    size_t n = std::min(max_frames, frames - off);
    std::vector<uint8_t> p(12 + n * align);
    uint32_t ts = base_ + uint32_t(b.frame + off);
    bool marker = off == 0 && (first_ || b.discontinuity || b.frame != expected_);
    p[0] = 0x80;
    p[1] = uint8_t(96 | (marker ? 0x80 : 0));
    p[2] = uint8_t(seq_ >> 8);
    p[3] = uint8_t(seq_++);
    for (unsigned k = 0; k < 4; ++k) {
      p[4 + k] = uint8_t(ts >> (24 - 8 * k));
      p[8 + k] = uint8_t(ssrc_ >> (24 - 8 * k));
    }
    for (size_t j = 0; j < n * align; j += 2) {
      p[12 + j] = b.pcm[off * align + j + 1];
      p[13 + j] = b.pcm[off * align + j];
    }
    out.push_back(std::move(p));
    off += n;
    first_ = false;
  }
  expected_ = b.frame + frames;
  return out;
}
std::pair<uint64_t, uint64_t> Rtp::send(const Block &b, uint64_t &bytes) {
  uint64_t good = 0, bad = 0;
  for (auto &p : packetize(b)) {
    ssize_t n = sendto(fd_, p.data(), p.size(), MSG_DONTWAIT | MSG_NOSIGNAL,
                       reinterpret_cast<sockaddr *>(&dest_), len_);
    if (n == ssize_t(p.size())) {
      ++good;
      ++packet_count_;
      octet_count_ += uint32_t(p.size() - 12);
      bytes += p.size();
    } else
      ++bad;
  }
  anchor_ = b.captured;
  anchor_frame_ = b.frame + b.pcm.size() / (2 * channels_);
  if (packet_count_ && seconds(rtcp_at_) >= rtcp_interval_)
    send_rtcp(bytes);
  return {good, bad};
}
std::vector<uint8_t> Rtp::rtcp_packet(bool bye) const {
  std::vector<uint8_t> p(28, 0);
  auto put = [&](size_t off, uint32_t value) {
    for (unsigned k = 0; k < 4; ++k)
      p[off + k] = uint8_t(value >> (24 - 8 * k));
  };
  p[0] = 0x80;
  p[1] = 200;
  p[3] = 6;
  put(4, ssrc_);
  auto now = std::chrono::system_clock::now().time_since_epoch();
  int64_t us = std::chrono::duration_cast<std::chrono::microseconds>(now).count();
  put(8, uint32_t(us / 1000000 + 2208988800ULL));
  put(12, uint32_t((uint64_t(us % 1000000) << 32) / 1000000));
  put(16, base_ + uint32_t(anchor_frame_ + uint64_t(seconds(anchor_) * rate_)));
  put(20, packet_count_);
  put(24, octet_count_);
  std::string cname = "audiocast-" + std::to_string(ssrc_);
  size_t sdes = (11 + cname.size() + 3) & ~size_t(3), offset = p.size();
  p.resize(offset + sdes, 0);
  p[offset] = 0x81;
  p[offset + 1] = 202;
  p[offset + 3] = uint8_t(sdes / 4 - 1);
  put(offset + 4, ssrc_);
  p[offset + 8] = 1;
  p[offset + 9] = uint8_t(cname.size());
  std::copy(cname.begin(), cname.end(), p.begin() + offset + 10);
  if (bye) {
    offset = p.size();
    p.resize(offset + 8, 0);
    p[offset] = 0x81;
    p[offset + 1] = 203;
    p[offset + 3] = 1;
    put(offset + 4, ssrc_);
  }
  return p;
}
void Rtp::send_rtcp(uint64_t &bytes, bool bye) {
  auto packet = rtcp_packet(bye);
  ssize_t n = sendto(fd_, packet.data(), packet.size(), MSG_DONTWAIT | MSG_NOSIGNAL,
                     reinterpret_cast<sockaddr *>(&rtcp_dest_), len_);
  if (n > 0)
    bytes += uint64_t(n);
  rtcp_at_ = Clock::now();
  // Randomized reporting interval; no receiver report feedback/RTT estimate is implemented.
  rtcp_interval_ = 2.5 + (uint32_t(ssrc_ + packet_count_) % 5000) / 1000.0;
}
std::string sdp(const Config &c) {
  std::ostringstream s;
  s << "v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\ns=webOS AudioCast\r\nc=IN "
    << (c.destination.find(':') != std::string::npos ? "IP6 " : "IP4 ") << c.destination
    << "\r\nt=0 0\r\nm=audio " << c.port << " RTP/AVP 96\r\na=rtpmap:96 L16/" << c.fmt.rate << "/"
    << c.fmt.channels << "\r\na=rtcp:" << (c.port == 65535 ? 65534 : c.port + 1)
    << "\r\na=sendonly\r\n";
  return s.str();
}
} // namespace ac
