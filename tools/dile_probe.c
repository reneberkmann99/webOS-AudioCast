// SPDX-License-Identifier: MIT
// Bounded MStar DILE upload probe. No ALSA, routing, speaker-volume writes or audio files.
// Explicit --gain-test-127 permits ONE five-second upload-gain test, restored to 12+0.
// --restore-upload-gain-12 is the separately supervised rollback path, without capture.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t halted = 0;
static pthread_mutex_t metrics = PTHREAD_MUTEX_INITIALIZER;
static uint64_t calls = 0, total_bytes = 0, samples = 0, interval_samples = 0;
static double sum_sq = 0, interval_sum = 0, peak = 0;
static unsigned min_bytes = 0, max_bytes = 0;
static int16_t first[16];
static unsigned first_n = 0;
static void on_term(int sig) { (void)sig; halted = 1; }

static int pcm_callback(const void *pcm, int bytes) {
    if (halted || bytes <= 0) return 0;
    if (!pcm || bytes > 65536) return 1;
    pthread_mutex_lock(&metrics);
    ++calls; total_bytes += (unsigned)bytes;
    if (!min_bytes || (unsigned)bytes < min_bytes) min_bytes = (unsigned)bytes;
    if ((unsigned)bytes > max_bytes) max_bytes = (unsigned)bytes;
    const int16_t *data = (const int16_t *)pcm;
    for (int i = 0; i < bytes / 2; ++i) {
        double value = data[i] / 32768.0;
        sum_sq += value * value; interval_sum += value * value;
        ++samples; ++interval_samples;
        if (fabs(value) > peak) peak = fabs(value);
        if (first_n < 16) first[first_n++] = data[i];
    }
    pthread_mutex_unlock(&metrics);
    return 0;
}

static unsigned char *inspect_upload(void *lib) {
    void *symbol = dlsym(lib, "HAL_AUDIO_PCM_GetInfo");
    Dl_info image;
    if (!symbol || !dladdr(symbol, &image)) return NULL;
    uintptr_t offset = (uintptr_t)symbol - (uintptr_t)image.dli_fbase;
    if ((offset & ~(uintptr_t)1) != 0x80a9c) return NULL; // Exact TV firmware only.
    unsigned char *shared = *(unsigned char **)((unsigned char *)image.dli_fbase + 0x1ff844);
    if (!shared) { fprintf(stderr, "UPLOAD_STATE not mapped\n"); return NULL; }
    fprintf(stderr, "UPLOAD_STATE main_gain=%u fine_gain=%u enabled=%u input_index=%u\n",
        shared[0x24f], shared[0x250], shared[0x368], *(uint32_t *)(shared + 0x364));
    return shared;
}

int main(int argc, char **argv) {
    int restore_only = argc == 2 && strcmp(argv[1], "--restore-upload-gain-12") == 0;
    int gain_test = argc == 4 && strcmp(argv[3], "--gain-test-127") == 0;
    if (argc > 4 || (argc == 4 && !gain_test)) return 2;
    int channel = argc > 1 ? atoi(argv[1]) : 0;
    int seconds = argc > 2 ? atoi(argv[2]) : 5;
    if (channel != 0 || seconds < 1 || seconds > 15) { fprintf(stderr, "Only the recovered channel 0 and a 1..15 second probe are permitted\n"); return 2; }
    if (gain_test && seconds != 5) return 2;
    signal(SIGTERM, on_term); signal(SIGINT, on_term); signal(SIGALRM, on_term);
    alarm((unsigned)seconds + 5);
    void *lib = dlopen("libdile_audio_pcm.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 3; }
    typedef int (*reg_fn)(int, int (*)(const void *, int));
    typedef int (*upload_fn)(int);
    typedef int (*gain_fn)(uint16_t);
    reg_fn reg = (reg_fn)dlsym(lib, "DILE_AUDIO_PCM_RegSendPCMCallback");
    upload_fn start = (upload_fn)dlsym(lib, "DILE_AUDIO_PCM_StartUpload");
    upload_fn finish = (upload_fn)dlsym(lib, "DILE_AUDIO_PCM_StopUpload");
    gain_fn set_gain = (gain_fn)dlsym(lib, "DILE_AUDIO_PCM_SetVolume");
    if (!reg || !start || !finish) { fprintf(stderr, "Required symbols unavailable\n"); return 4; }
    if ((gain_test || restore_only) && !set_gain) return 4;
    if (gain_test || restore_only) {
        Dl_info image;
        void *info = dlsym(lib, "HAL_AUDIO_PCM_GetInfo");
        if (!info || !dladdr(info, &image) ||
            (((uintptr_t)info - (uintptr_t)image.dli_fbase) & ~(uintptr_t)1) != 0x80a9c) {
            fprintf(stderr, "Unrecognized firmware; no gain write permitted\n"); return 4;
        }
    }
    if (restore_only) {
        // Only used if the supervised test did not emit a verified restoration record.
        int restored = set_gain(12);
        unsigned char *state = inspect_upload(lib);
        int verified = restored == 0 && state && state[0x24f] == 12 && state[0x250] == 0;
        fprintf(stderr, "GAIN_ROLLBACK original=12 fine=0 result=%d verified=%d\n", restored, verified);
        fflush(stderr);
        return verified ? 0 : 8;
    }
    fprintf(stderr, "dile_probe: channel=0 duration=%d gain_test=%d no routing/speaker-volume writes\n", seconds, gain_test);
    int result = start(channel);
    fprintf(stderr, "StartUpload(%d)=%d\n", channel, result);
    if (result) return 5;
    unsigned char *state = inspect_upload(lib); // Never call GetInfo on an uninitialized HAL.
    if (gain_test && (!state || state[0x24f] != 12 || state[0x250] != 0)) {
        fprintf(stderr, "Unexpected upload-gain state; refusing gain write\n"); finish(channel); return 8;
    }
    result = reg(channel, pcm_callback);
    fprintf(stderr, "RegSendPCMCallback(%d)=%d\n", channel, result);
    if (result) { finish(channel); return 6; }
    int changed = 0, gain_verified = 1;
    if (gain_test) {
        changed = 1; // Mark before calling: a partial setter still requires restoration.
        fprintf(stderr, "GAIN_WRITE_BEGIN original=12 fine=0 requested=127\n");
        fflush(stderr);
        result = set_gain(127);
        gain_verified = result == 0 && state[0x24f] == 127 && state[0x250] == 0;
        fprintf(stderr, "GAIN_TEST original=12 fine=0 requested=127 result=%d verified=%d\n", result, gain_verified);
        if (!gain_verified) halted = 1;
    }
    for (int tick = 0; tick < seconds * 10 && !halted; ++tick) {
        usleep(100000);
        if (tick % 10 == 9) {
            pthread_mutex_lock(&metrics);
            double rms = interval_samples ? sqrt(interval_sum / interval_samples) : 0;
            fprintf(stderr, "progress t=%d calls=%llu bytes=%llu rms_1s=%.6f\n", (tick + 1) / 10,
                (unsigned long long)calls, (unsigned long long)total_bytes, rms);
            interval_samples = 0; interval_sum = 0;
            pthread_mutex_unlock(&metrics); fflush(stderr);
        }
    }
    halted = 1;
    int restored = 1;
    if (changed) {
        int restore_result = set_gain(12);
        restored = restore_result == 0 && state[0x24f] == 12 && state[0x250] == 0;
        fprintf(stderr, "GAIN_RESTORE_CALL result=%d verified=%d\n", restore_result, restored);
    }
    reg(channel, NULL);
    result = finish(channel);
    fprintf(stderr, "StopUpload(%d)=%d\n", channel, result);
    if (changed) {
        restored = restored && state[0x24f] == 12 && state[0x250] == 0;
        fprintf(stderr, "GAIN_RESTORED original=12 fine=0 verified=%d\n", restored);
    }
    pthread_mutex_lock(&metrics);
    double rms = samples ? sqrt(sum_sq / samples) : 0;
    fprintf(stderr, "RESULT calls=%llu bytes=%llu min=%u max=%u rms=%.6f peak=%.6f\n",
        (unsigned long long)calls, (unsigned long long)total_bytes, min_bytes, max_bytes, rms, peak);
    fprintf(stderr, "FIRST"); for (unsigned i = 0; i < first_n; ++i) fprintf(stderr, " %d", first[i]); fprintf(stderr, "\n");
    pthread_mutex_unlock(&metrics); fflush(stderr);
    alarm(0);
    // Vendor background workers may outlive StopUpload; process exit, not dlclose, reclaims them.
    return result == 0 && restored && gain_verified ? 0 : 7;
}
