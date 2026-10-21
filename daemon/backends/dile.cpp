// SPDX-License-Identifier: MIT
// Experimental playback upload for ONE inspected MStar firmware. No ALSA, mixer or routing.
#include "dile.hpp"
#include <array>
#include <cerrno>
#include <climits>
#include <csignal>
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <iostream>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/utsname.h>
namespace ac {
namespace {
constexpr uint64_t HAL_FNV = UINT64_C(0xb0fdbf9d4268bc24);
constexpr uint64_t DILE_FNV = UINT64_C(0xbc3aecd8ce3c0831);
const char *HAL_FILE = "/usr/lib/libhal_msd96alvx.so.0.1.0";
const char *DILE_FILE = "/usr/lib/libdile_audio_pcm.so.1.0.0";
std::atomic<bool> restore_ok{true};

// Compatibility fingerprint, NOT a cryptographic authenticity check. Vendor code is never shipped.
bool fingerprint(const std::string &path, size_t expected_size, uint64_t expected_hash) {
  struct stat st{};
  if (stat(path.c_str(), &st) || !S_ISREG(st.st_mode) || st.st_size != off_t(expected_size))
    return false;
  std::ifstream f(path.c_str(), std::ios::binary);
  uint64_t hash = UINT64_C(14695981039346656037);
  size_t count = 0;
  char buffer[8192];
  while (f) {
    f.read(buffer, sizeof(buffer));
    size_t n = size_t(f.gcount());
    count += n;
    if (count > expected_size) return false;
    for (size_t i = 0; i < n; ++i) hash = (hash ^ uint8_t(buffer[i])) * UINT64_C(1099511628211);
  }
  return !f.bad() && count == expected_size && hash == expected_hash;
}
bool real_firmware() {
  struct utsname u{};
  return sizeof(void *) == 4 && uname(&u) == 0 && std::string(u.machine).find("armv7") == 0 &&
         fingerprint(HAL_FILE, 2019988, HAL_FNV) && fingerprint(DILE_FILE, 3960, DILE_FNV);
}
const char *fixture(bool allow) {
  const char *p = getenv("AUDIOCAST_DILE_TEST_LIBRARY");
  if (p && *p) {
    if (!allow || p[0] != '/')
      throw std::runtime_error("DILE ABI fixture requires explicit --allow-test-source and absolute path");
    return p;
  }
  return nullptr;
}
std::string lock_path(bool allow) {
  const char *p = allow ? getenv("AUDIOCAST_DILE_TEST_LOCK") : nullptr;
  return p && *p ? p : "/var/lib/webos-audiocast/dile-upload.lock";
}
class OwnerLock {
  int fd_ = -1;
public:
  explicit OwnerLock(bool allow) {
    fd_ = ::open(lock_path(allow).c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    struct stat st{};
    if (fd_ < 0 || fstat(fd_, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
        (st.st_mode & 0777) != 0600 || st.st_nlink != 1 || flock(fd_, LOCK_EX | LOCK_NB)) {
      close_fd(fd_);
      throw std::runtime_error("DILE upload is owned by another AudioCast process, or its private lock is unsafe");
    }
  }
  ~OwnerLock() { close_fd(fd_); }
};
// Conservative conflict guard. Mere existence of the normally idle Bluetooth service is NOT a conflict.
// This is not a proof that all firmware consumers have been detected: coexistence remains unqualified.
void reject_conflicts(bool fake) {
  if (fake) return;
  DIR *proc = opendir("/proc");
  if (!proc) throw std::runtime_error("cannot inspect DILE upload ownership");
  std::string conflict;
  while (dirent *entry = readdir(proc)) {
    if (entry->d_name[0] < '0' || entry->d_name[0] > '9') continue;
    std::string base = std::string("/proc/") + entry->d_name;
    std::string comm = read_file(base + "/comm", 128);
    if (comm.find("acr2") == 0) { conflict = "ACR consumer is running"; break; }
    if (comm.find("btaudiosrc") != 0) continue;
    DIR *fds = opendir((base + "/fd").c_str());
    if (!fds) { conflict = "cannot inspect Bluetooth upload ownership"; break; }
    while (dirent *fd = readdir(fds)) {
      char target[512];
      ssize_t n = readlink((base + "/fd/" + fd->d_name).c_str(), target, sizeof(target)-1);
      if (n <= 0) continue;
      target[n] = 0;
      std::string name(target);
      if (name.find("/dev/mad") == 0 || name.find("/dev/snd/") == 0 ||
          name.find("/dev/utopia") == 0 || name == "/dev/mem") {
        conflict = "Bluetooth service owns vendor audio hardware"; break;
      }
    }
    closedir(fds);
    if (!conflict.empty()) break;
  }
  closedir(proc);
  if (!conflict.empty()) throw std::runtime_error("DILE capture refused: " + conflict + "; coexistence is unqualified");
}
struct Api {
  void *lib = nullptr;
  uintptr_t base = 0;
  unsigned char *(*fake_state)() = nullptr;
  int (*start)(int) = nullptr;
  int (*reg)(int, int (*)(const void *, int)) = nullptr;
  int (*stop)(int) = nullptr;
  int (*gain)(uint16_t) = nullptr;
  bool fake = false;
  template <typename T> T symbol(const char *name) {
    void *p = dlsym(lib, name);
    if (!p) throw std::runtime_error(std::string("DILE symbol unavailable: ") + name);
    return reinterpret_cast<T>(p);
  }
  void load(bool allow) {
    const char *path = fixture(allow);
    fake = path != nullptr;
    if (!fake && !real_firmware())
      throw std::runtime_error("DILE is restricted to the exact inspected ARMv7 MStar library build");
    lib = dlopen(fake ? path : "libdile_audio_pcm.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib) throw std::runtime_error("DILE vendor library unavailable");
    start = symbol<int (*)(int)>("DILE_AUDIO_PCM_StartUpload");
    reg = symbol<int (*)(int, int (*)(const void *, int))>("DILE_AUDIO_PCM_RegSendPCMCallback");
    stop = symbol<int (*)(int)>("DILE_AUDIO_PCM_StopUpload");
    gain = symbol<int (*)(uint16_t)>("DILE_AUDIO_PCM_SetVolume");
    if (fake) {
      fake_state = symbol<unsigned char *(*)()>("AudioCast_DILE_TestState");
    } else {
      void *info = dlsym(lib, "HAL_AUDIO_PCM_GetInfo");
      Dl_info image{}, wrapper{};
      if (!info || !dladdr(info, &image) || !dladdr(reinterpret_cast<void *>(gain), &wrapper) ||
          (((uintptr_t(info) - uintptr_t(image.dli_fbase)) & ~uintptr_t(1)) != 0x80a9c) ||
          !fingerprint(image.dli_fname, 2019988, HAL_FNV) ||
          !fingerprint(wrapper.dli_fname, 3960, DILE_FNV))
        throw std::runtime_error("DILE loaded library differs from the inspected firmware; no HAL calls permitted");
      base = uintptr_t(image.dli_fbase);
    }
  }
  unsigned char *state() const {
    // Reading the mapped pointer is safe ONLY after StartUpload or the restoring gain setter.
    // Never invoke HAL_AUDIO_PCM_GetInfo on an uninitialized HAL: it segfaults on this firmware.
    return fake_state ? fake_state() : (base ? *reinterpret_cast<unsigned char **>(base + 0x1ff844) : nullptr);
  }
  // Intentionally no dlclose: the vendor keeps internal workers after StopUpload.
};
class DileCapture;
std::mutex owner_mu;
DileCapture *owner = nullptr;
std::atomic<bool> callback_missed{false};

class DileCapture : public Capture {
  bool allow_;
  Api api_;
  std::unique_ptr<OwnerLock> lock_;
  unsigned char *state_ = nullptr;
  bool started_ = false, registered_ = false, touched_ = false, cleanup_done_ = false;
  std::string recovery_, cleanup_error_;
  std::atomic<bool> accepting_{false}, expired_{false}, malformed_{false};
  std::atomic<uint32_t> callback_calls_{0}, callback_bytes_{0}, dropped_bytes_{0};
  std::mutex wait_mu_, timer_mu_, cleanup_mu_;
  std::condition_variable ready_, timer_wake_;
  struct Chunk {
    // 0=free, 1=writer owns, 2=ready, 3=reader owns. Never overwrite a reader's slot.
    std::atomic<unsigned> state{0}, length{0}, sequence{0};
    std::array<uint8_t,8192> pcm{};
  };
  std::array<Chunk,8> chunks_{};
  std::array<uint8_t,8192> staging_{};
  size_t staging_head_ = 0, staging_size_ = 0;
  unsigned next_sequence_ = 0;
  std::atomic<bool> discontinuity_{false};
  bool timer_cancel_ = false;
  std::thread timer_;
  Clock::time_point deadline_;

  static int callback(const void *pcm, int bytes) {
    std::unique_lock<std::mutex> owner_lock(owner_mu, std::try_to_lock);
    if (!owner_lock.owns_lock()) { callback_missed = true; return 0; }
    DileCapture *self = owner;
    if (!self || !self->accepting_) return 0;
    if (!pcm || bytes <= 0 || bytes > 8192 || (bytes % 4)) { self->malformed_ = true; self->ready_.notify_one(); return 0; }
    ++self->callback_calls_; self->callback_bytes_ += unsigned(bytes);
    Chunk *target = nullptr;
    for (auto &chunk : self->chunks_) {
      unsigned expected = 0;
      if (chunk.state.compare_exchange_strong(expected,1)) { target = &chunk; break; }
    }
    if (!target) {
      // Only a full queue drops audio. A consumer copying a different slot cannot cause loss.
      Chunk *oldest = nullptr;
      unsigned sequence = UINT_MAX;
      for (auto &chunk : self->chunks_)
        if (chunk.state.load() == 2 && chunk.sequence.load() < sequence) {
          oldest = &chunk; sequence = chunk.sequence.load();
        }
      unsigned expected = 2;
      if (oldest && oldest->state.compare_exchange_strong(expected,1)) {
        target = oldest; self->dropped_bytes_ += target->length.load();
      }
      self->discontinuity_ = true;
    }
    if (!target) { self->dropped_bytes_ += unsigned(bytes); return 0; }
    memcpy(target->pcm.data(),pcm,size_t(bytes));
    target->length = unsigned(bytes);
    target->sequence = self->next_sequence_++;
    target->state = 2;
    self->ready_.notify_one();
    return 0;
  }
  bool has_chunk() const {
    for (const auto &chunk : chunks_) if (chunk.state.load() == 2) return true;
    return false;
  }
  bool take_chunk() {
    for (unsigned retry=0;retry<chunks_.size();++retry) {
      Chunk *oldest = nullptr;
      unsigned sequence = UINT_MAX;
      for (auto &chunk : chunks_)
        if (chunk.state.load() == 2 && chunk.sequence.load() < sequence) {
          oldest = &chunk; sequence = chunk.sequence.load();
        }
      if (!oldest) return false;
      unsigned expected = 2;
      if (!oldest->state.compare_exchange_strong(expected,3)) continue;
      if (oldest->sequence.load() != sequence) { oldest->state = 2; continue; }
      staging_size_ = oldest->length.load(); staging_head_ = 0;
      memcpy(staging_.data(),oldest->pcm.data(),staging_size_);
      oldest->state = 0;
      return true;
    }
    return false;
  }
  void cleanup_hal() {
    std::lock_guard<std::mutex> cleanup_lock(cleanup_mu_);
    if (cleanup_done_) return;
    cleanup_done_ = true;
    accepting_ = false;
    {
      std::lock_guard<std::mutex> owner_lock(owner_mu);
      if (owner == this) owner = nullptr;
    }
    int unregistered = registered_ && api_.reg ? api_.reg(0, nullptr) : 0;
    registered_ = false;
    bool restored = true;
    if (touched_) {
      int result = api_.gain(12);
      restored = result == 0 && state_ && state_[0x24f] == 12 && state_[0x250] == 0;
      std::cerr << "DILE_GAIN_RESTORE result=" << result << " verified=" << restored << "\n";
    }
    int stopped = started_ && api_.stop ? api_.stop(0) : 0;
    started_ = false;
    if (touched_) restored = restored && state_ && state_[0x24f] == 12 && state_[0x250] == 0;
    if (!restored || unregistered || stopped) {
      restore_ok = false;
      cleanup_error_ = "DILE cleanup/restoration failed; stop experimentation and run owner-only recovery or reboot";
    } else {
      if (touched_ && !recovery_.empty() && unlink(recovery_.c_str()) && errno != ENOENT) {
        cleanup_error_ = "DILE gain restored, but recovery record could not be removed";
      }
      restore_ok = true;
    }
    if (touched_) std::cerr << "DILE_GAIN_RESTORED original=12 fine=0 verified=" << bool(restore_ok) << "\n";
    ready_.notify_all();
  }
public:
  explicit DileCapture(bool allow) : allow_(allow) {}
  ~DileCapture() override {
    try { close(); } catch (const std::exception &e) { std::cerr << "AudioCast: " << e.what() << "\n"; }
  }
  void open(const Config &c) override {
    if (c.device != "mstar-upload:0" || c.fmt.sample != "S16_LE" || c.fmt.rate != 48000 || c.fmt.channels != 2 ||
        !c.confirmed || !c.dile_gain_confirmed || c.autostart)
      throw std::runtime_error("DILE requires explicit mstar-upload:0, verified playback, upload-gain consent, 48kHz stereo S16 and manual start");
    if (c.dile_recovery_file.empty()) throw std::runtime_error("DILE private recovery path is missing");
    recovery_ = c.dile_recovery_file;
    if (access(recovery_.c_str(), F_OK) == 0)
      throw std::runtime_error("pending DILE gain recovery record; restore it before another capture");
    unsigned limit = c.dile_continuous
                         ? 604800000U  // 7 days: continuous until explicit stop / daemon exit
                         : std::min(30000U, std::min(c.dile_limit_ms, dile_test_limit_ms(allow_)));
    if (!limit) throw std::runtime_error("DILE capture grant expired");
    deadline_ = Clock::now() + std::chrono::milliseconds(limit);
    // Reject unsupported hosts/test overrides before creating a hardware ownership lock.
    const char *fake_path = fixture(allow_);
    if (!fake_path && !real_firmware())
      throw std::runtime_error("DILE is restricted to the exact inspected ARMv7 MStar library build");
    lock_.reset(new OwnerLock(allow_));
    api_.load(allow_);
    reject_conflicts(api_.fake);
    {
      std::lock_guard<std::mutex> owner_lock(owner_mu);
      if (owner) throw std::runtime_error("DILE callback already owned in this process");
      owner = this;
    }
    try {
      // Mark before the call: even a partially failed StartUpload must be stopped.
      started_ = true;
      if (api_.start(0)) throw std::runtime_error("DILE StartUpload failed");
      state_ = api_.state();
      if (!state_ || state_[0x24f] != 12 || state_[0x250] != 0)
        throw std::runtime_error("DILE original upload gain differs from inspected 12+0; no gain change permitted");
      atomic_file(recovery_, Json({{"schema",1},{"pid",getpid()},{"main_gain",12},{"fine_gain",0},
                                  {"firmware_fingerprint","b0fdbf9d4268bc24"}}).dump() + "\n");
      touched_ = true; restore_ok = false;
      if (api_.gain(127) || state_[0x24f] != 127 || state_[0x250] != 0)
        throw std::runtime_error("DILE upload-gain initialization/readback failed");
      std::cerr << "DILE_GAIN_SET requested=127 original=12 fine=0 verified=1\n";
      accepting_ = true;
      registered_ = true; // Partial registration failure must also unregister.
      if (api_.reg(0, callback)) throw std::runtime_error("DILE PCM callback registration failed");
      callback_missed = false;
      timer_ = std::thread([this] {
        std::unique_lock<std::mutex> timer_lock(timer_mu_);
        if (timer_wake_.wait_until(timer_lock, deadline_, [this] { return timer_cancel_; })) return;
        expired_ = true;
        timer_lock.unlock();
        cleanup_hal();
      });
    } catch (...) {
      cleanup_hal();
      if (!cleanup_error_.empty()) throw std::runtime_error(cleanup_error_);
      throw;
    }
  }
  std::vector<uint8_t> read(unsigned frames, bool &disc) override {
    if (expired_ || Clock::now() >= deadline_) throw std::runtime_error("DILE experimental capture time limit reached; explicit start required");
    if (malformed_) throw std::runtime_error("DILE delivered malformed PCM; capture stopped");
    size_t wanted = std::min(size_t(frames) * 4, staging_.size());
    std::vector<uint8_t> data(wanted);
    if (!staging_size_ && !take_chunk()) {
      std::unique_lock<std::mutex> wait_lock(wait_mu_);
      ready_.wait_for(wait_lock, std::chrono::milliseconds(25), [this] { return has_chunk() || !accepting_ || expired_ || malformed_; });
      take_chunk();
    }
    if (expired_ || Clock::now() >= deadline_) throw std::runtime_error("DILE experimental capture time limit reached; explicit start required");
    if (malformed_ || !accepting_) throw std::runtime_error("DILE capture ended or delivered invalid PCM; explicit start required");
    size_t n = std::min(wanted, staging_size_); n -= n % 4;
    if (!n) { disc = false; return {}; }
    memcpy(data.data(),staging_.data() + staging_head_,n);
    staging_head_ += n; staging_size_ -= n;
    bool dropped = discontinuity_.exchange(false);
    disc = callback_missed.exchange(false) || dropped;
    data.resize(n);
    return data;
  }
  void close() override {
    {
      std::lock_guard<std::mutex> timer_lock(timer_mu_);
      timer_cancel_ = true;
    }
    timer_wake_.notify_one();
    if (timer_.joinable()) timer_.join();
    cleanup_hal();
    lock_.reset();
    if (!cleanup_error_.empty()) throw std::runtime_error(cleanup_error_);
  }
  unsigned long delay_frames() const override {
    size_t bytes = staging_size_;
    for (const auto &chunk : chunks_) if (chunk.state.load() == 2) bytes += chunk.length.load();
    return static_cast<unsigned long>(bytes / 4);
  }
  Json diagnostics() const override {
    return {{"callback_calls",uint64_t(callback_calls_)},{"callback_bytes",uint64_t(callback_bytes_)},
            {"dropped_bytes",uint64_t(dropped_bytes_)},{"expired",bool(expired_)}};
  }
};
} // namespace
Json dile_inventory() {
  bool available = real_firmware();
  return {{"id","dile"},{"available",available},{"experimental",true},{"capture_limit_seconds",30},
          {"reason",available ? "Exact inspected MStar build; explicit upload-gain consent and bounded capture required" :
                               "Only the exact inspected ARMv7 MStar vendor-library build is supported"}};
}
std::unique_ptr<Capture> make_dile_capture(bool allow) { return std::unique_ptr<Capture>(new DileCapture(allow)); }
bool dile_restore_verified() { return restore_ok; }
unsigned dile_test_limit_ms(bool allow) {
  const char *p = allow && getenv("AUDIOCAST_DILE_TEST_LIBRARY") ? getenv("AUDIOCAST_DILE_TEST_LIMIT_MS") : nullptr;
  if (!p || !*p) return 30000;
  char *end = nullptr;
  unsigned long n = strtoul(p, &end, 10);
  return end && !*end && n > 0 && n <= 30000 ? unsigned(n) : 30000;
}
Json restore_dile_gain(const std::string &dir, bool allow) {
  std::string path = dir + "/dile-recovery.json";
  struct stat st{};
  if (lstat(path.c_str(), &st)) {
    if (errno == ENOENT) return {{"restored",true},{"pending_recovery",false},{"hal_called",false}};
    throw std::runtime_error("cannot inspect DILE recovery record");
  }
  if (!S_ISREG(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 0777) != 0600 || st.st_size > 4096 || st.st_nlink != 1)
    throw std::runtime_error("unsafe DILE recovery record");
  Json record = Json::parse(read_file(path,4096));
  int64_t pid = record.at("pid").get<int64_t>();
  if (record.at("schema") != 1 || record.at("main_gain") != 12 || record.at("fine_gain") != 0 ||
      record.at("firmware_fingerprint") != "b0fdbf9d4268bc24" || pid < 1 || pid > INT_MAX)
    throw std::runtime_error("unrecognized DILE recovery transaction; no gain write permitted");
  if (kill(pid_t(pid),0) == 0 || errno == EPERM) throw std::runtime_error("DILE transaction owner is still alive; refusing rollback");
  OwnerLock lock(allow);
  Api api; api.load(allow);
  reject_conflicts(api.fake);
  int result = api.gain(12);
  unsigned char *state = api.state();
  bool restored = result == 0 && state && state[0x24f] == 12 && state[0x250] == 0;
  int stopped = restored ? api.stop(0) : -1;
  restored = restored && stopped == 0 && state[0x24f] == 12 && state[0x250] == 0;
  restore_ok = restored;
  if (!restored) throw std::runtime_error("DILE rollback failed; stop experimentation and reboot/recover TV");
  if (unlink(path.c_str())) throw std::runtime_error("DILE rollback succeeded, but recovery record removal failed");
  return {{"restored",true},{"pending_recovery",false},{"original_main_gain",12},{"original_fine_gain",0}};
}
} // namespace ac
