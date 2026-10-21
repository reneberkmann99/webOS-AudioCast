// SPDX-License-Identifier: MIT
#include "backends/capture.hpp"
#include "backends/dile.hpp"
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
void private_probe_dir(const std::string &dir) {
  if (mkdir(dir.c_str(),0700) && errno != EEXIST) throw std::runtime_error("cannot create private probe directory");
  struct stat st{};
  if (lstat(dir.c_str(),&st) || !S_ISDIR(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 0777) != 0700)
    throw std::runtime_error("probe state directory must be private, owned, and not a symlink");
}
int probe_dile(const std::string &dir, bool allow_test, bool gain_consent, bool playback_consent, unsigned duration) {
  if (!gain_consent || !playback_consent) throw std::runtime_error("--probe-dile requires --dile-gain-confirmed and --playback-confirmed");
  if (duration < 1 || duration > 30) throw std::runtime_error("DILE probe duration must be 1..30 seconds");
  private_probe_dir(dir);
  ac::Config c;
  c.backend = "dile"; c.device = "mstar-upload:0"; c.confirmed = true; c.dile_gain_confirmed = true;
  c.dile_recovery_file = dir + "/dile-recovery.json";
  // The main loop ends just before duration; a small cleanup margin avoids racing its watchdog.
  c.dile_limit_ms = std::min(30000U,duration * 1000 + 250U);
  signal(SIGTERM,on_signal); signal(SIGINT,on_signal); signal(SIGPIPE,SIG_IGN);
  auto source = ac::make_capture("dile",allow_test);
  auto began = ac::Clock::now();
  source->open(c);
  double opening_seconds = ac::seconds(began);
  uint64_t frames = 0, discontinuities = 0;
  std::vector<double> square(2,0), peaks(2,0);
  while (!halted && ac::seconds(began) < duration - 0.04) {
    bool disc = false;
    auto data = source->read(480,disc);
    if (disc) ++discontinuities;
    if (data.empty()) continue;
    std::vector<double> peak;
    auto rms = ac::rms(data,c.fmt,peak);
    size_t count = data.size()/4;
    for (size_t i=0;i<2;++i) { square[i] += rms[i]*rms[i]*count; peaks[i] = std::max(peaks[i],peak[i]); }
    frames += count;
  }
  source->close();
  for (double &value : square) value = frames ? sqrt(value/frames) : 0;
  std::cout << "DILE_PROBE_RESULT " << ac::Json({{"backend","dile"},{"device",c.device},
      {"captured_frames",frames},{"rms",square},{"peak",peaks},{"discontinuities",discontinuities},
      {"gain_restored",ac::dile_restore_verified()},{"audio_sent",false},{"audio_saved",false},
      {"duration_seconds",ac::seconds(began)},{"opening_seconds",opening_seconds},
      {"backend_diagnostics",source->diagnostics()},{"format",c.fmt.json()}}).dump() << std::endl;
  return frames && ac::dile_restore_verified() ? 0 : 1;
}
} // namespace
int main(int argc, char **argv) {
  try {
    std::string dir = "/var/lib/webos-audiocast", probe;
    bool test = false, bootstrap = false, selftest = false, dile_probe = false, dile_restore = false;
    bool gain_consent = false, playback_consent = false;
    unsigned duration = 5;
    for (int i = 1; i < argc; ++i) {
      std::string a = argv[i];
      if (a == "--version") {
        std::cout << "webOS AudioCast 0.1.1\n";
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
      else if (a == "--probe-dile")
        dile_probe = true;
      else if (a == "--restore-dile-gain")
        dile_restore = true;
      else if (a == "--dile-gain-confirmed")
        gain_consent = true;
      else if (a == "--playback-confirmed")
        playback_consent = true;
      else if (a == "--seconds" && i + 1 < argc) {
        std::string text = argv[++i];
        if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos || text.size() > 2)
          throw std::runtime_error("duration must be an integer from 1 to 30");
        duration = unsigned(std::stoul(text));
        if (duration < 1 || duration > 30) throw std::runtime_error("duration must be 1..30");
      }
      else
        throw std::runtime_error("usage: audiocastd [--state-dir PATH] [--bootstrap] "
                                 "[--allow-test-source] [--self-test] [--version]");
    }
    if (dile_probe && dile_restore) throw std::runtime_error("choose probe or rollback, not both");
    if (dile_probe) return probe_dile(dir,test,gain_consent,playback_consent,duration);
    if (dile_restore) { private_probe_dir(dir); std::cout << ac::restore_dile_gain(dir,test).dump() << std::endl; return 0; }
    if (gain_consent || playback_consent) throw std::runtime_error("DILE consent switches apply only to --probe-dile");
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
