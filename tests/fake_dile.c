// SPDX-License-Identifier: MIT
// Explicit host-only vendor ABI double. Never linked into the TV application.
#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static unsigned char state[0x400];
static pthread_t worker;
static int worker_started;
static _Atomic int running;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static int (*callback)(const void *, int);
static void trace(const char *text) {
    const char *path = getenv("FAKE_DILE_TRACE");
    if (!path) return;
    FILE *file = fopen(path,"a");
    if (file) {fprintf(file,"%s\n",text);fclose(file);}
}
__attribute__((constructor)) static void init(void) {
    state[0x24f] = getenv("FAKE_DILE_WRONG_GAIN") ? 13 : 12;
}
unsigned char *AudioCast_DILE_TestState(void) {return state;}
static void *emit(void *unused) {
    (void)unused;
    int16_t pcm[2304];
    unsigned counter=0;
    while (running) {
        usleep(getenv("FAKE_DILE_BURST") ? 200 : 24000);
        pthread_mutex_lock(&mu);
        int gain = state[0x24f];
        for (unsigned i=0;i<1152;i++) {
            int16_t value = gain == 127 && !getenv("FAKE_DILE_SILENCE") ? (int16_t)((int)(counter++ % 200)-100)*64 : 0;
            pcm[2*i]=value; pcm[2*i+1]=value;
        }
        if (callback) callback(pcm,getenv("FAKE_DILE_MALFORMED") ? 3 : (int)sizeof(pcm));
        pthread_mutex_unlock(&mu);
    }
    return NULL;
}
int DILE_AUDIO_PCM_StartUpload(int channel) {
    trace("start");
    if (channel != 0) return -1;
    state[0x368]=1;
    if (getenv("FAKE_DILE_START_FAIL")) return -1;
    running=1;
    if (pthread_create(&worker,NULL,emit,NULL)) return -1;
    worker_started=1;
    return 0;
}
int DILE_AUDIO_PCM_RegSendPCMCallback(int channel,int (*cb)(const void *,int)) {
    if (channel != 0) return -1;
    trace(cb ? "register" : "unregister");
    pthread_mutex_lock(&mu);
    int (*old)(const void *,int)=callback;
    callback=cb;
    pthread_mutex_unlock(&mu);
    if (cb && getenv("FAKE_DILE_BURST")) {
        // Deterministically overflow before registration returns and the reader can start.
        int16_t pcm[2304]={0};
        for (int i=0;i<24;i++) cb(pcm,sizeof(pcm));
    }
    if (!cb && old && getenv("FAKE_DILE_LATE_CALLBACK")) {
        int16_t pcm[4]={1,2,3,4}; old(pcm,sizeof(pcm));
    }
    return cb && getenv("FAKE_DILE_REGISTER_FAIL") ? -1 : 0;
}
int DILE_AUDIO_PCM_SetVolume(uint16_t gain) {
    char text[64];snprintf(text,sizeof(text),"gain:%u",gain);trace(text);
    pthread_mutex_lock(&mu);
    state[0x24f]=gain & 255;state[0x250]=gain>>8;
    pthread_mutex_unlock(&mu);
    if (gain == 127 && getenv("FAKE_DILE_GAIN_FAIL")) return -1;
    if (gain == 12 && getenv("FAKE_DILE_RESTORE_FAIL")) return -1;
    return 0;
}
int DILE_AUDIO_PCM_StopUpload(int channel) {
    trace("stop");
    if (channel != 0) return -1;
    running=0;
    if (worker_started) {pthread_join(worker,NULL);worker_started=0;}
    state[0x368]=0;
    return 0;
}
