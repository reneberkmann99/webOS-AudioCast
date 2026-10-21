// SPDX-License-Identifier: MIT
#include "common.hpp"
#include <dlfcn.h>
#include <fcntl.h>
#include <regex>
#include <sstream>
#include <sys/stat.h>
#include <sys/utsname.h>
namespace ac {
Config Config::parse(const Json &j, const Config &old, bool test) {
  if (!j.is_object())
    throw std::runtime_error("config must be an object");
  Config c = old;
  Json merged = old.json();
  for (auto it = j.begin(); it != j.end(); ++it) {
    if (!merged.contains(it.key()))
      throw std::runtime_error("unknown config field: " + it.key());
    merged[it.key()] = it.value();
  }
  try {
    c.backend = merged.at("backend").get<std::string>();
    c.device = merged.at("device").get<std::string>();
    c.transport = merged.at("transport").get<std::string>();
    c.destination = merged.at("destination").get<std::string>();
    c.profile = merged.at("profile").get<std::string>();
    c.fmt.sample = merged.at("sample_format").get<std::string>();
    for (const auto &k : {"rate", "channels", "destination_port", "packet_ms"})
      if (!merged.at(k).is_number_integer() || merged.at(k).get<int64_t>() < 0 ||
          merged.at(k).get<int64_t>() > 1000000)
        throw std::runtime_error("invalid numeric field");
    c.fmt.rate = merged.at("rate").get<unsigned>();
    c.fmt.channels = merged.at("channels").get<unsigned>();
    c.port = merged.at("destination_port").get<unsigned>();
    c.packet_ms = merged.at("packet_ms").get<unsigned>();
    c.http_lan = merged.at("http_lan").get<bool>();
    c.autostart = merged.at("autostart").get<bool>();
    c.confirmed = merged.at("playback_confirmed").get<bool>();
  } catch (const Json::exception &) {
    throw std::runtime_error("incorrect config value type");
  }
  if (c.backend != "alsa" && !(test && c.backend == "test-tone"))
    throw std::runtime_error("backend unavailable; only ALSA is implemented on TV");
  if (!c.device.empty() && !std::regex_match(c.device, std::regex("hw:[0-9]{1,3},[0-9]{1,3}")))
    throw std::runtime_error(
        "device must be explicit hw:CARD,DEVICE (no default or microphone fallback)");
  if (c.transport != "rtp" && c.transport != "http" && c.transport != "both")
    throw std::runtime_error("invalid transport");
  if (c.fmt.sample != "S16_LE" && c.fmt.sample != "S32_LE")
    throw std::runtime_error("unsupported sample format");
  if (c.fmt.sample != "S16_LE" && c.transport != "http")
    throw std::runtime_error("RTP/L16 requires S16_LE capture; no implicit conversion");
  if (c.fmt.rate != 32000 && c.fmt.rate != 44100 && c.fmt.rate != 48000 && c.fmt.rate != 96000)
    throw std::runtime_error("rate must be 32000,44100,48000,96000");
  if (c.fmt.channels < 1 || c.fmt.channels > 2)
    throw std::runtime_error("only mono/stereo supported");
  if (c.port < 1024 || c.port > 65535)
    throw std::runtime_error("destination port must be 1024..65535");
  if (c.packet_ms != 5 && c.packet_ms != 10 && c.packet_ms != 20)
    throw std::runtime_error("packet_ms must be 5,10,20");
  if (c.profile != "low-latency" && c.profile != "stable")
    throw std::runtime_error("invalid buffering profile");
  if (c.destination.size() > 253 ||
      !std::regex_match(c.destination, std::regex("[A-Za-z0-9][A-Za-z0-9.:-]{0,252}")))
    throw std::runtime_error("invalid destination hostname/IP");
  if (c.backend != old.backend || c.device != old.device || c.fmt.json() != old.fmt.json())
    c.confirmed = j.value("playback_confirmed", false);
  return c;
}
std::string random_token(size_t count) {
  int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    throw std::runtime_error("cannot open OS random source");
  std::vector<unsigned char> b(count);
  size_t n = 0;
  while (n < count) {
    ssize_t k = read(fd, b.data() + n, count - n);
    if (k < 0 && errno == EINTR)
      continue;
    if (k <= 0) {
      close(fd);
      throw std::runtime_error("OS random read failed");
    }
    n += size_t(k);
  }
  close(fd);
  static const char *h = "0123456789abcdef";
  std::string s;
  for (auto x : b) {
    s += h[x >> 4];
    s += h[x & 15];
  }
  return s;
}
void atomic_file(const std::string &p, const std::string &text) {
  std::string tmp = p + ".tmp." + random_token(6);
  int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0)
    throw std::runtime_error("cannot create state file");
  size_t n = 0;
  while (n < text.size()) {
    ssize_t r = write(fd, text.data() + n, text.size() - n);
    if (r < 0 && errno == EINTR)
      continue;
    if (r <= 0) {
      close(fd);
      unlink(tmp.c_str());
      throw std::runtime_error("state write failed");
    }
    n += size_t(r);
  }
  int rc = fsync(fd);
  close(fd);
  if (rc < 0 || rename(tmp.c_str(), p.c_str()) < 0) {
    unlink(tmp.c_str());
    throw std::runtime_error("state commit failed");
  }
}
static std::string proc_pcm_path() {
  const char *override = getenv("AUDIOCAST_PROC_PCM");
  return override && *override ? override : "/proc/asound/pcm";
}
// A capture PCM is hazardous when its ALSA driver is the MStar MAD decoder. Opening those devices
// has been observed to wedge the kernel in uninterruptible sleep on real LG MStar hardware; they
// carry no playback audio. Keyed off the /proc/asound/pcm driver id/name, never a card index.
std::string capture_hazard(const std::string &device) {
  std::smatch dm;
  if (!std::regex_match(device, dm, std::regex("hw:([0-9]{1,3}),([0-9]{1,3})")))
    return "";
  int card = std::stoi(dm[1]), dev = std::stoi(dm[2]);
  std::istringstream in(read_file(proc_pcm_path()));
  std::string line;
  std::regex re("^([0-9]+)-([0-9]+): (.*)");
  while (std::getline(in, line)) {
    std::smatch m;
    if (!std::regex_search(line, m, re) || std::stoi(m[1]) != card || std::stoi(m[2]) != dev)
      continue;
    std::string name = m[3];
    if (name.find("mad_pcm") != std::string::npos || name.find("MStar MAD") != std::string::npos)
      return "MStar MAD decoder capture device: opening it wedges the kernel driver in "
             "uninterruptible sleep (confirmed on real hardware) and carries no playback audio. "
             "Blocked. Use the vendor loopback backend; override only with "
             "AUDIOCAST_ALLOW_MSTAR_MAD_CAPTURE=1 if you accept the hang risk.";
    return "";
  }
  return "";
}
Json discover() {
  void *alsa = dlopen("libasound.so.2", RTLD_LAZY | RTLD_LOCAL);
  bool alsa_present = alsa != nullptr;
  if (alsa)
    dlclose(alsa);
  Json dev = Json::array();
  bool mstar_mad = false;
  std::istringstream in(read_file(proc_pcm_path()));
  std::string line;
  std::regex re("^([0-9]+)-([0-9]+): (.*)");
  while (std::getline(in, line)) {
    std::smatch m;
    if (line.find("capture") == std::string::npos || !std::regex_search(line, m, re))
      continue;
    std::string name = m[3];
    std::string id =
        "hw:" + std::to_string(std::stoi(m[1])) + "," + std::to_string(std::stoi(m[2]));
    std::string hazard = capture_hazard(id);
    if (!hazard.empty())
      mstar_mad = true;
    bool candidate = hazard.empty() && name.find("CAPTURE_TEST") != std::string::npos;
    Json entry = {{"id", id},          {"name", name},
                  {"backend", "alsa"}, {"candidate", candidate},
                  {"verified", false}, {"safe_to_open", hazard.empty()}};
    if (!hazard.empty())
      entry["unsafe_reason"] = hazard;
    dev.push_back(entry);
  }
  Json backends =
      Json::array({{{"id", "alsa"}, {"available", alsa_present}},
                   {{"id", "pulse-monitor"},
                    {"available", false},
                    {"reason", "TV media may bypass PulseAudio (confirmed on MStar: "
                               "sinks stay SUSPENDED during playback); monitor silent"}},
                   {{"id", "vendor"},
                    {"available", false},
                    {"reason", "DILE/MStar MAD HAL loopback not implemented"}}});
  std::string warning = "Inventory is not proof of playback capture. Test against known authorized "
                        "TV playback. Never select microphone/default input.";
  if (mstar_mad)
    warning += " MStar MAD decoder capture devices are present and BLOCKED: opening them hangs the "
               "kernel driver and yields no playback audio. No ALSA capture backend works on this "
               "SoC; a vendor loopback backend is required.";
  return {
      {"backends", backends}, {"devices", dev}, {"soc_mstar_mad", mstar_mad}, {"warning", warning}};
}
Json system_info() {
  struct utsname u;
  memset(&u, 0, sizeof(u));
  uname(&u);
  return {{"kernel", std::string(u.sysname) + " " + u.release},
          {"architecture", u.machine},
          {"webos_release", read_file("/etc/webos-release", 4096)},
          {"os_release", read_file("/etc/os-release", 4096)},
          {"cpuinfo", read_file("/proc/cpuinfo", 12000)},
          {"alsa_cards", read_file("/proc/asound/cards", 12000)},
          {"alsa_pcm", read_file("/proc/asound/pcm", 12000)}};
}
} // namespace ac
