// SPDX-License-Identifier: MIT
#include "../engine.hpp"
#include "http.hpp"
#include "rtp.hpp"
#include <ctime>
#include <fcntl.h>
#include <memory>
#include <poll.h>
#include <sstream>
#include <sys/resource.h>
#include <sys/stat.h>
namespace ac {
namespace {
struct Client {
  int fd = -1;
  bool audio = false;
  std::string input, output;
  size_t sent = 0;
  Clock::time_point born = Clock::now(), progress = Clock::now();
};
std::string chunk(const std::vector<uint8_t> &b) {
  std::ostringstream s;
  s << std::hex << b.size() << "\r\n";
  std::string out = s.str();
  out.append(reinterpret_cast<const char *>(b.data()), b.size());
  return out + "\r\n";
}
Json metadata(const Config &c) {
  return {{"sample_format", c.fmt.sample},  {"rate", c.fmt.rate},
          {"channels", c.fmt.channels},     {"interleaved", true},
          {"byte_order", "little-endian"},  {"transport", "HTTP chunked raw PCM"},
          {"rtp_payload_type", 96},         {"rtp_byte_order", "big-endian"},
          {"rtp_max_datagram_bytes", 1200}, {"packet_ms", c.packet_ms},
          {"end_to_end_ms", nullptr}};
}
} // namespace
void Engine::network_loop() {
  int server = -1;
  bool bound_lan = false;
  std::vector<Client> clients;
  std::unique_ptr<Rtp> rtp;
  unsigned gen = ~0U;
  Clock::time_point bind_retry = Clock::now() - std::chrono::seconds(10), rtp_retry = bind_retry,
                    rate_start = Clock::now();
  uint64_t tx = 0;
  double prev_cpu = 0;
  auto previous = Clock::now();
  while (!quit_) {
    Config c = config();
    if (server >= 0 && bound_lan != c.http_lan) {
      close_fd(server);
      for (auto &x : clients)
        close_fd(x.fd);
      clients.clear();
    }
    if (server < 0 && seconds(bind_retry) >= 5) {
      bind_retry = Clock::now();
      try {
        server = listen_tcp(c.http_lan, 7070);
        bound_lan = c.http_lan;
        log(c.http_lan ? "Authenticated HTTP audio listener available on LAN:7070"
                       : "HTTP audio listener loopback-only:7070");
      } catch (const std::exception &e) {
        stat("last_error", e.what());
        log(e.what());
      }
    }
    unsigned current = generation_;
    if (current != gen) {
      gen = current;
      rtp.reset();
      rtp_retry = Clock::now() - std::chrono::seconds(10);
      for (auto &x : clients)
        close_fd(x.fd);
      clients.clear();
    }
    bool streaming_now = streaming();
    if (!streaming_now) {
      for (auto &x : clients)
        if (x.audio)
          close_fd(x.fd);
      rtp.reset();
    }
    bool want_rtp = streaming_now && c.transport != "http";
    if (want_rtp && !rtp && seconds(rtp_retry) >= 5) {
      rtp_retry = Clock::now();
      try {
        rtp.reset(new Rtp);
        rtp->open(c);
      } catch (const std::exception &e) {
        rtp.reset();
        stat("last_error", e.what());
        log(std::string("RTP: ") + e.what());
      }
    }
    if (server >= 0) {
      for (unsigned i = 0; i < 4; ++i) {
        int fd = accept(server, nullptr, nullptr);
        if (fd < 0)
          break;
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        int size = 16384;
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
        if (clients.size() >= 8) {
          close(fd);
          continue;
        }
        Client x;
        x.fd = fd;
        clients.push_back(std::move(x));
      }
    }
    for (auto &x : clients) {
      if (x.fd < 0)
        continue;
      if (!x.audio && x.output.empty()) {
        try {
          for (unsigned i = 0; i < 4; ++i) {
            char b[2048];
            ssize_t n = recv(x.fd, b, sizeof(b), MSG_DONTWAIT);
            if (n > 0)
              x.input.append(b, size_t(n));
            else {
              if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
                close_fd(x.fd);
              break;
            }
          }
          if (x.fd < 0)
            continue;
          Request req;
          if (parse_request(x.input, req)) {
            if (!authorized(req.authorization, stream_token))
              x.output = response(401, "{\"error\":\"stream bearer token required\"}");
            else if (req.method != "GET")
              x.output = response(400, "{\"error\":\"GET only\"}");
            else if (req.path == "/metadata")
              x.output = response(200, metadata(c).dump());
            else if (req.path == "/stream.sdp")
              x.output = response(200, sdp(c), "application/sdp");
            else if (req.path == "/audio") {
              if (!streaming_now || c.transport == "rtp")
                x.output = response(409, "{\"error\":\"HTTP streaming not active\"}");
              else {
                x.audio = true;
                x.output =
                    "HTTP/1.1 200 OK\r\nContent-Type: "
                    "application/octet-stream\r\nTransfer-Encoding: chunked\r\nCache-Control: "
                    "no-store\r\nConnection: close\r\nX-Audio-Rate: " +
                    std::to_string(c.fmt.rate) +
                    "\r\nX-Audio-Channels: " + std::to_string(c.fmt.channels) +
                    "\r\nX-Audio-Format: " + c.fmt.sample + "\r\n\r\n";
              }
            } else
              x.output = response(404, "{\"error\":\"not found\"}");
            x.input.clear();
          } else if (seconds(x.born) > 1)
            close_fd(x.fd);
        } catch (...) {
          close_fd(x.fd);
        }
      }
      if (x.audio && x.fd >= 0) {
        char b;
        ssize_t n = recv(x.fd, &b, 1, MSG_PEEK | MSG_DONTWAIT);
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) || n > 0)
          close_fd(x.fd);
      }
    }
    // Drain newest bounded capture blocks; no client or UDP send can block capture.
    Block block;
    for (unsigned i = 0; i < 8 && ring.pop(block); ++i) {
      if (!streaming_now || current != generation_)
        continue;
      double age = seconds(block.captured) * 1000;
      if (age > 100) {
        add("network_stale_drops", 1);
        continue;
      }
      if (rtp) {
        auto counts = rtp->send(block, tx);
        add("rtp_packets", counts.first);
        add("rtp_errors", counts.second);
      }
      if (c.transport != "rtp") {
        std::string data = chunk(block.pcm);
        for (auto &x : clients)
          if (x.fd >= 0 && x.audio) {
            if (x.sent) {
              x.output.erase(0, x.sent);
              x.sent = 0;
            }
            if (x.output.size() + data.size() > 32768) {
              close_fd(x.fd);
              add("http_slow_disconnects", 1);
            } else
              x.output += data;
          }
      }
      stat("capture_to_network_ms", age);
    }
    unsigned active = 0;
    for (auto &x : clients) {
      if (x.fd < 0)
        continue;
      if (x.audio)
        ++active;
      if (x.sent < x.output.size()) {
        ssize_t n =
            send(x.fd, x.output.data() + x.sent, std::min(size_t(16384), x.output.size() - x.sent),
                 MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n > 0) {
          x.sent += size_t(n);
          if (x.audio)
            tx += uint64_t(n);
          x.progress = Clock::now();
        } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
          close_fd(x.fd);
        if (seconds(x.progress) > 2)
          close_fd(x.fd);
      }
      if (x.sent == x.output.size() && !x.output.empty()) {
        if (!x.audio)
          close_fd(x.fd);
        x.output.clear();
        x.sent = 0;
      }
    }
    clients.erase(
        std::remove_if(clients.begin(), clients.end(), [](const Client &x) { return x.fd < 0; }),
        clients.end());
    stat("http_clients", active);
    if (seconds(rate_start) >= 1) {
      double dt = seconds(rate_start);
      stat("throughput_bps", double(tx) * 8 / dt);
      tx = 0;
      rate_start = Clock::now();
      struct rusage usage{};
      getrusage(RUSAGE_SELF, &usage);
      double cpu = usage.ru_utime.tv_sec + usage.ru_utime.tv_usec / 1e6 + usage.ru_stime.tv_sec +
                   usage.ru_stime.tv_usec / 1e6;
      stat("cpu_percent", std::max(0.0, (cpu - prev_cpu) / seconds(previous) * 100));
      prev_cpu = cpu;
      previous = Clock::now();
      std::istringstream mem(read_file("/proc/self/statm", 256));
      unsigned long total = 0, resident = 0;
      mem >> total >> resident;
      stat("rss_kb", resident * uint64_t(sysconf(_SC_PAGESIZE)) / 1024);
      std::string output = "unknown";
      struct ::stat platform_stat{};
      if (::stat((dir_ + "/platform.json").c_str(), &platform_stat) == 0 &&
          std::time(nullptr) - platform_stat.st_mtime < 25) {
        try {
          Json platform = Json::parse(read_file(dir_ + "/platform.json", 4096));
          if (platform.value("returnValue", false) && platform.contains("soundOutput") &&
              platform["soundOutput"].is_string())
            output = platform["soundOutput"].get<std::string>().substr(0, 128);
        } catch (...) { /* Firmware/API absent: report unknown, never invent an output. */
        }
      }
      stat("audio_output", output);
    }
    bool pending_output = std::any_of(clients.begin(), clients.end(), [](const Client &x) {
      return x.fd >= 0 && x.sent < x.output.size();
    });
    // Capture arrival wakes this immediately; no 500Hz idle/config-copy busy polling.
    ring.wait(pending_output ? 2 : (streaming_now ? 20 : 50));
  }
  for (auto &x : clients)
    close_fd(x.fd);
  close_fd(server);
}
} // namespace ac
