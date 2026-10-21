// SPDX-License-Identifier: MIT
// Dynamic ALSA ABI: only libasound.so.2 at runtime, no copied vendor code/headers.
#include "capture.hpp"
#include "dile.hpp"
#include <cerrno>
#include <dlfcn.h>
namespace ac {
struct snd_pcm_t;
struct snd_pcm_hw_params_t;
typedef unsigned long uframes;
typedef long sframes;
class AlsaCapture : public Capture {
  void *lib_ = nullptr;
  snd_pcm_t *pcm_ = nullptr;
  Format fmt_;
  unsigned long delay_ = 0;
  template <typename T> T sym(const char *n) {
    void *p = dlsym(lib_, n);
    if (!p)
      throw std::runtime_error(std::string("ALSA symbol unavailable: ") + n);
    return reinterpret_cast<T>(p);
  }
  int (*close_)(snd_pcm_t *) = nullptr;
  int (*wait_)(snd_pcm_t *, int) = nullptr;
  sframes (*read_)(snd_pcm_t *, void *, uframes) = nullptr;
  int (*prepare_)(snd_pcm_t *) = nullptr;
  int (*resume_)(snd_pcm_t *) = nullptr;
  int (*delay_fn_)(snd_pcm_t *, sframes *) = nullptr;
  const char *(*error_)(int) = nullptr;
  void check(int r, const char *n) {
    if (r < 0)
      throw std::runtime_error(std::string(n) + ": " + (error_ ? error_(r) : "ALSA error"));
  }

  void recover(int error, bool &disc) {
    if (error == -EINTR || error == -EAGAIN)
      return;
    if (error == -EPIPE) {
      check(prepare_(pcm_), "prepare after capture overrun");
      disc = true;
      return;
    }
    if (error == -ESTRPIPE) {
      // snd_pcm_recover can sleep/retry indefinitely in suspend. Try once, then close/reopen.
      int result = resume_(pcm_);
      if (result == -EAGAIN)
        throw std::runtime_error("capture suspended; releasing device and retrying with backoff");
      if (result < 0)
        check(prepare_(pcm_), "prepare after capture suspend");
      disc = true;
      return;
    }
    check(error, "capture device failure");
  }

public:
  ~AlsaCapture() {
    if (pcm_ && close_)
      close_(pcm_);
    if (lib_)
      dlclose(lib_);
  }
  void open(const Config &c) override {
    fmt_ = c.fmt;
    std::string hazard = capture_hazard(c.device);
    if (!hazard.empty() && !getenv("AUDIOCAST_ALLOW_MSTAR_MAD_CAPTURE"))
      throw std::runtime_error(hazard);
    lib_ = dlopen("libasound.so.2", RTLD_NOW | RTLD_LOCAL);
    if (!lib_)
      throw std::runtime_error("libasound.so.2 unavailable; install/vendor an ABI-compatible ALSA "
                               "library, never fallback to microphone");
    close_ = sym<int (*)(snd_pcm_t *)>("snd_pcm_close");
    wait_ = sym<int (*)(snd_pcm_t *, int)>("snd_pcm_wait");
    read_ = sym<sframes (*)(snd_pcm_t *, void *, uframes)>("snd_pcm_readi");
    prepare_ = sym<int (*)(snd_pcm_t *)>("snd_pcm_prepare");
    resume_ = sym<int (*)(snd_pcm_t *)>("snd_pcm_resume");
    delay_fn_ = sym<int (*)(snd_pcm_t *, sframes *)>("snd_pcm_delay");
    error_ = sym<const char *(*)(int)>("snd_strerror");
    check(sym<int (*)(snd_pcm_t **, const char *, int, int)>("snd_pcm_open")(
              &pcm_, c.device.c_str(), 1, 1),
          "capture open"); // CAPTURE, NONBLOCK. NEVER opens playback or mixer.
    snd_pcm_hw_params_t *p = nullptr;
    auto alloc = sym<int (*)(snd_pcm_hw_params_t **)>("snd_pcm_hw_params_malloc");
    auto freep = sym<void (*)(snd_pcm_hw_params_t *)>("snd_pcm_hw_params_free");
    check(alloc(&p), "hw params allocation");
    try {
      check(sym<int (*)(snd_pcm_t *, snd_pcm_hw_params_t *)>("snd_pcm_hw_params_any")(pcm_, p),
            "hw params");
      check(sym<int (*)(snd_pcm_t *, snd_pcm_hw_params_t *, int)>("snd_pcm_hw_params_set_access")(
                pcm_, p, 3),
            "interleaved capture");
      int format = sym<int (*)(const char *)>("snd_pcm_format_value")(fmt_.sample.c_str());
      if (format < 0)
        throw std::runtime_error("ALSA sample format unknown");
      check(sym<int (*)(snd_pcm_t *, snd_pcm_hw_params_t *, int)>("snd_pcm_hw_params_set_format")(
                pcm_, p, format),
            "sample format");
      check(sym<int (*)(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned)>(
                "snd_pcm_hw_params_set_channels")(pcm_, p, fmt_.channels),
            "channels");
      unsigned rate = fmt_.rate;
      int dir = 0;
      check(sym<int (*)(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned *, int *)>(
                "snd_pcm_hw_params_set_rate_near")(pcm_, p, &rate, &dir),
            "sample rate");
      if (rate != fmt_.rate)
        throw std::runtime_error("requested native rate unavailable; no automatic resampling");
      uframes period = std::max(1U, fmt_.rate * c.packet_ms / 1000);
      dir = 0;
      check(sym<int (*)(snd_pcm_t *, snd_pcm_hw_params_t *, uframes *, int *)>(
                "snd_pcm_hw_params_set_period_size_near")(pcm_, p, &period, &dir),
            "period");
      uframes buffer = period * (c.profile == "stable" ? 8 : 3);
      check(sym<int (*)(snd_pcm_t *, snd_pcm_hw_params_t *, uframes *)>(
                "snd_pcm_hw_params_set_buffer_size_near")(pcm_, p, &buffer),
            "buffer");
      check(sym<int (*)(snd_pcm_t *, snd_pcm_hw_params_t *)>("snd_pcm_hw_params")(pcm_, p),
            "apply capture params");
      freep(p);
      p = nullptr;
      check(prepare_(pcm_), "prepare capture");
    } catch (...) {
      if (p)
        freep(p);
      throw;
    }
  }
  std::vector<uint8_t> read(unsigned frames, bool &disc) override {
    disc = false;
    std::vector<uint8_t> b(size_t(frames) * fmt_.channels * fmt_.bytes());
    int w = wait_(pcm_, 25);
    if (w == 0)
      return {};
    if (w < 0) {
      recover(w, disc);
      return {};
    }
    sframes n = read_(pcm_, b.data(), frames);
    if (n == -EAGAIN)
      return {};
    if (n < 0) {
      recover(int(n), disc);
      return {};
    }
    if (n == 0)
      return {};
    b.resize(size_t(n) * fmt_.channels * fmt_.bytes());
    sframes delay = 0;
    if (delay_fn_(pcm_, &delay) >= 0)
      delay_ = delay > 0 ? static_cast<unsigned long>(delay) : 0;
    return b;
  }
  unsigned long delay_frames() const override { return delay_; }
};
class TestCapture : public Capture {
  Config c_;
  uint64_t frame_ = 0;
  Clock::time_point next_;

public:
  void open(const Config &c) override {
    c_ = c;
    next_ = Clock::now();
  }
  std::vector<uint8_t> read(unsigned n, bool &disc) override {
    disc = false;
    next_ += std::chrono::microseconds(uint64_t(n) * 1000000 / c_.fmt.rate);
    std::this_thread::sleep_until(next_);
    std::vector<uint8_t> b(size_t(n) * c_.fmt.channels * c_.fmt.bytes());
    for (unsigned i = 0; i < n; ++i) {
      double v = std::sin(2 * 3.141592653589793 * 440 * double(frame_++) / c_.fmt.rate) * .25;
      for (unsigned ch = 0; ch < c_.fmt.channels; ++ch) {
        size_t p = (size_t(i) * c_.fmt.channels + ch) * c_.fmt.bytes();
        if (c_.fmt.bytes() == 2) {
          uint16_t s = uint16_t(int16_t(v * 32767));
          b[p] = uint8_t(s);
          b[p + 1] = uint8_t(s >> 8);
        } else {
          uint32_t s = uint32_t(int32_t(v * 2147483647));
          for (unsigned k = 0; k < 4; ++k)
            b[p + k] = uint8_t(s >> (k * 8));
        }
      }
    }
    return b;
  }
  unsigned long delay_frames() const override { return 0; }
};
std::unique_ptr<Capture> make_capture(const std::string &name, bool test) {
  if (name == "alsa")
    return std::unique_ptr<Capture>(new AlsaCapture);
  if (name == "dile")
    return make_dile_capture(test);
  if (name == "test-tone" && test)
    return std::unique_ptr<Capture>(new TestCapture);
  throw std::runtime_error("capture backend unavailable");
}
Json probe_device(const std::string &device) {
  Config base;
  Config::parse({{"device", device}}, base, false);
  if (device.empty())
    throw std::runtime_error("explicit device required");
  std::string hazard = capture_hazard(device);
  if (!hazard.empty() && !getenv("AUDIOCAST_ALLOW_MSTAR_MAD_CAPTURE"))
    throw std::runtime_error(hazard);
  void *lib = dlopen("libasound.so.2", RTLD_NOW | RTLD_LOCAL);
  if (!lib)
    throw std::runtime_error("libasound.so.2 unavailable");
  snd_pcm_t *pcm = nullptr;
  snd_pcm_hw_params_t *params = nullptr;
  auto load = [&](const char *name) -> void * {
    void *fn = dlsym(lib, name);
    if (!fn)
      throw std::runtime_error(std::string("missing ALSA symbol: ") + name);
    return fn;
  };
  auto close_pcm = reinterpret_cast<int (*)(snd_pcm_t *)>(dlsym(lib, "snd_pcm_close"));
  auto free_params =
      reinterpret_cast<void (*)(snd_pcm_hw_params_t *)>(dlsym(lib, "snd_pcm_hw_params_free"));
  try {
    if (!close_pcm || !free_params)
      throw std::runtime_error("ALSA cleanup symbols unavailable");
    auto open_pcm =
        reinterpret_cast<int (*)(snd_pcm_t **, const char *, int, int)>(load("snd_pcm_open"));
    auto alloc =
        reinterpret_cast<int (*)(snd_pcm_hw_params_t **)>(load("snd_pcm_hw_params_malloc"));
    auto any = reinterpret_cast<int (*)(snd_pcm_t *, snd_pcm_hw_params_t *)>(
        load("snd_pcm_hw_params_any"));
    if (open_pcm(&pcm, device.c_str(), 1, 1) < 0 || alloc(&params) < 0 || any(pcm, params) < 0)
      throw std::runtime_error("cannot query selected capture device (missing, busy, or denied)");
    Json formats = Json::array(), rates = Json::array(), channels = Json::array();
    auto format_value = reinterpret_cast<int (*)(const char *)>(load("snd_pcm_format_value"));
    auto test_format = reinterpret_cast<int (*)(snd_pcm_t *, snd_pcm_hw_params_t *, int)>(
        load("snd_pcm_hw_params_test_format"));
    auto test_rate = reinterpret_cast<int (*)(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned, int)>(
        load("snd_pcm_hw_params_test_rate"));
    auto test_channels = reinterpret_cast<int (*)(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned)>(
        load("snd_pcm_hw_params_test_channels"));
    for (const char *f : {"S16_LE", "S16_BE", "S24_LE", "S24_3LE", "S32_LE", "FLOAT_LE"}) {
      int value = format_value(f);
      if (value >= 0 && test_format(pcm, params, value) == 0)
        formats.push_back(f);
    }
    for (unsigned r : {8000U, 16000U, 22050U, 32000U, 44100U, 48000U, 88200U, 96000U, 192000U})
      if (test_rate(pcm, params, r, 0) == 0)
        rates.push_back(r);
    for (unsigned c = 1; c <= 8; ++c)
      if (test_channels(pcm, params, c) == 0)
        channels.push_back(c);
    free_params(params);
    params = nullptr;
    close_pcm(pcm);
    pcm = nullptr;
    dlclose(lib);
    return {{"device", device},
            {"formats", formats},
            {"rates_tested_supported", rates},
            {"channels_tested_supported", channels},
            {"note", "Independent capability constraints, not all combinations guaranteed; no "
                     "capture params applied or audio read. Does not prove playback provenance."}};
  } catch (...) {
    if (params && free_params)
      free_params(params);
    if (pcm && close_pcm)
      close_pcm(pcm);
    dlclose(lib);
    throw;
  }
}
} // namespace ac
