// SPDX-License-Identifier: MIT
#include "backends/capture.hpp"
#include "engine.hpp"
#include "network/http.hpp"
#include "network/rtp.hpp"
#include <csignal>
#include <fcntl.h>
#include <iostream>
#include <sys/file.h>
#include <sys/stat.h>
namespace {
volatile sig_atomic_t halted = 0;
void on_signal(int) {
  halted = 1;
}
} // namespace
int main(int argc, char **argv) {
  try {
    std::string dir = "/var/lib/webos-audiocast", probe;
    bool test = false, bootstrap = false, selftest = false;
    for (int i = 1; i < argc; ++i) {
      std::string a = argv[i];
      if (a == "--version") {
        std::cout << "webOS AudioCast 0.1.0\n";
        return 0;
      }
      if (a == "--state-dir" && i + 1 < argc)
        dir = argv[++i];
      else if (a == "--probe-device" && i + 1 < argc)
        probe = argv[++i];
      else if (a == "--allow-test-source")
        test = true;
      else if (a == "--bootstrap")
        bootstrap = true;
      else if (a == "--self-test")
        selftest = true;
      else
        throw std::runtime_error("usage: audiocastd [--state-dir PATH] [--bootstrap] "
                                 "[--allow-test-source] [--self-test] [--version]");
    }
    if (!probe.empty()) {
      std::cout << ac::probe_device(probe).dump(2) << std::endl;
      return 0;
    }
    if (selftest) {
      ac::Config c;
      ac::Config good = ac::Config::parse({{"device", "hw:0,13"}}, c, false);
      if (good.confirmed)
        throw std::runtime_error("source must not auto-verify");
      ac::Rtp r;
      r.open(c);
      ac::Block b;
      b.frame = 123;
      b.pcm = {0x34, 0x12, 0x00, 0x80};
      auto packets = r.packetize(b);
      if (packets.size() != 1 || packets[0].size() != 16 || packets[0][12] != 0x12 ||
          packets[0][13] != 0x34 || packets[0][14] != 0x80 || packets[0][15] != 0)
        throw std::runtime_error("L16 endian self-test");
      ac::Ring ring(2);
      ring.push(b);
      ring.push(b);
      ring.push(b);
      if (ring.drops() != 1)
        throw std::runtime_error("bounded ring self-test");
      std::cout << "Self-tests passed: config consent, RTP L16 endian, bounded ring\n";
      return 0;
    }
    umask(0077);
    ac::Engine engine(dir, test);
    if (bootstrap) {
      std::cout << ac::Json({{"returnValue", true},
                             {"control_token", engine.control_token},
                             {"stream_token", engine.stream_token},
                             {"control_url", "http://127.0.0.1:7071"}})
                       .dump()
                << std::endl;
      return 0;
    }
    int lock =
        open((dir + "/daemon.lock").c_str(), O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) < 0)
      throw std::runtime_error("another daemon instance owns state directory");
    ac::atomic_file(dir + "/daemon.pid", std::to_string(getpid()) + "\n");
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGPIPE, SIG_IGN);
    int control = ac::listen_tcp(false, 7071);
    engine.launch();
    ac::control_loop(engine, control, [] { return halted != 0; });
    engine.stop();
    engine.shutdown();
    ac::close_fd(control);
    unlink((dir + "/daemon.pid").c_str());
    close(lock);
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "AudioCast: " << e.what() << "\n";
    return 1;
  }
}
