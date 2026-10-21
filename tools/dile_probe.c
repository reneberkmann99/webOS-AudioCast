// SPDX-License-Identifier: MIT
// DILE audio-PCM capture probe for LG MStar webOS. Bounded, single-shot, non-invasive:
// registers a callback, StartUpload, counts bytes for N seconds, StopUpload. No audio written
// to disk. Verifies the recovered callback signature (const void *pcm, int bytes).
#include <dlfcn.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stop = 0;
static uint64_t calls = 0, total_bytes = 0, max_bytes = 0, min_bytes = ~0ULL;
static double sum_sq = 0.0;
static uint64_t rms_n = 0;
static int16_t first[16];
static int first_n = 0;
static int first_fired = 0;
static volatile double sec_sum_sq = 0.0;
static volatile uint64_t sec_n = 0;

static void on_term(int sig) {
  (void)sig;
  stop = 1;
}

static int pcm_callback(const void *pcm, int bytes) {
  if (stop || bytes <= 0)
    return 0;
  ++calls;
  if (!first_fired) {
    first_fired = 1;
    fprintf(stderr, "CALLBACK_FIRED first_bytes=%d\n", bytes);
    fflush(stderr);
  }
  uint64_t b = (uint64_t)bytes;
  total_bytes += b;
  if (b > max_bytes)
    max_bytes = b;
  if (b < min_bytes)
    min_bytes = b;
  const int16_t *s = (const int16_t *)pcm;
  int n = bytes / 2;
  // Cheap RMS over a bounded subset (first ~1M samples) to avoid doing heavy work on the HAL thread.
  for (int i = 0; i < n && rms_n < 1000000; ++i, ++rms_n) {
    double x = s[i] / 32768.0;
    sum_sq += x * x;
    sec_sum_sq += x * x;
    ++sec_n;
  }
  if (first_n < 16) {
    int take = n < (16 - first_n) ? n : (16 - first_n);
    memcpy(first + first_n, s, (size_t)take * sizeof(int16_t));
    first_n += take;
  }
  return 0;
}

int main(int argc, char **argv) {
  int channel = 0, seconds = 5;
  if (argc > 1)
    channel = atoi(argv[1]);
  if (argc > 2)
    seconds = atoi(argv[2]);
  if (seconds < 1 || seconds > 60)
    seconds = 5;
  fprintf(stderr, "dile_probe: channel=%d seconds=%d\n", channel, seconds);

  void *lib = dlopen("libdile_audio_pcm.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!lib) {
    fprintf(stderr, "dlopen libdile_audio_pcm.so.1 failed: %s\n", dlerror());
    return 2;
  }
  typedef int (*reg_fn)(int, int (*)(const void *, int));
  typedef int (*upload_fn)(int);
  reg_fn reg = (reg_fn)dlsym(lib, "DILE_AUDIO_PCM_RegSendPCMCallback");
  upload_fn start = (upload_fn)dlsym(lib, "DILE_AUDIO_PCM_StartUpload");
  upload_fn stopf = (upload_fn)dlsym(lib, "DILE_AUDIO_PCM_StopUpload");
  if (!reg || !start || !stopf) {
    fprintf(stderr, "missing DILE_AUDIO_PCM symbols: %s\n", dlerror());
    dlclose(lib);
    return 3;
  }

  signal(SIGTERM, on_term);
  signal(SIGINT, on_term);

  int r;
  // btaudiosrc order: StartUpload first, then RegSendPCMCallback (matches its disassembly).
  r = start(channel);
  fprintf(stderr, "StartUpload(%d) = %d\n", channel, r);
  if (r != 0) {
    fprintf(stderr, "StartUpload failed\n");
    dlclose(lib);
    return 5;
  }
  r = reg(channel, pcm_callback);
  fprintf(stderr, "RegSendPCMCallback(%d) = %d\n", channel, r);
  if (r != 0) {
    fprintf(stderr, "register failed\n");
    dlclose(lib);
    return 4;
  }

  for (int i = 0; i < seconds * 10 && !stop; ++i) {
    usleep(100000); // 100ms * 10 = 1s
    if (i % 10 == 0) {
      double srms = sec_n ? sqrt(sec_sum_sq / sec_n) : 0.0;
      sec_sum_sq = 0.0;
      sec_n = 0;
      fprintf(stderr, "progress t=%d calls=%llu bytes=%llu rms_1s=%.5f\n", i / 10,
              (unsigned long long)calls, (unsigned long long)total_bytes, srms);
      fflush(stderr);
    }
  }

  stopf(channel);
  fprintf(stderr, "StopUpload(%d) done\n", channel);

  double rms = rms_n ? sqrt(sum_sq / rms_n) : 0.0;
  fprintf(stderr, "RESULT calls=%llu bytes=%llu min=%llu max=%llu rms=%.5f first_fired=%d\n",
          (unsigned long long)calls, (unsigned long long)total_bytes, (unsigned long long)min_bytes,
          (unsigned long long)max_bytes, rms, first_fired);
  fprintf(stderr, "FIRST");
  for (int i = 0; i < first_n; ++i)
    fprintf(stderr, " %d", first[i]);
  fprintf(stderr, "\n");
  fflush(stderr);
  if (rms == 0.0)
    fprintf(stderr, "NOTE: rms==0 means silence (nothing playing) or wrong channel/format.\n");
  dlclose(lib);
  return 0;
}
