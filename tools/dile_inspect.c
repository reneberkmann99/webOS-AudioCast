// SPDX-License-Identifier: MIT
// Read-only, firmware-specific inspection of the MStar PCM upload state.
// No StartUpload, mixer, volume, routing, ALSA open, or PCM recording calls.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>

int main(void) {
    // Load the same dependency group as btaudiosrc/acr2; the board HAL alone has unresolved sibling symbols.
    void *lib = dlopen("libdile_audio_pcm.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 2; }
    void *symbol = dlsym(lib, "HAL_AUDIO_PCM_GetInfo");
    Dl_info image;
    if (!symbol || !dladdr(symbol, &image)) { fprintf(stderr, "GetInfo missing\n"); return 3; }
    uintptr_t offset = (uintptr_t)symbol - (uintptr_t)image.dli_fbase;
    fprintf(stderr, "library=%s GetInfo_offset=0x%lx\n", image.dli_fname, (unsigned long)offset);
    // Exact offset recovered from this TV's firmware; refuse every other build.
    if ((offset & ~(uintptr_t)1) != 0x80a9c) { fprintf(stderr, "Different firmware; refusing offset-based inspection\n"); return 4; }
    // This firmware's GetInfo dereferences an uninitialized HAL mapping. Never call it here.
    unsigned char *shared = *(unsigned char **)((unsigned char *)image.dli_fbase + 0x1ff844);
    if (!shared) { fprintf(stderr, "Audio state not mapped; refusing HAL getter/init. Use only an approved bounded capture probe.\n"); return 5; }
    // HAL_AUDIO_PCM_SetVolume stores only these two upload-volume bytes. Read, never write.
    fprintf(stderr, "UPLOAD_STATE main_gain=%u fine_gain=%u enabled=%u input_index=%u init=%u\n",
        shared[0x24f], shared[0x250], shared[0x368], *(uint32_t *)(shared + 0x364), shared[0]);
    // Exit without unloading a vendor HAL containing background workers.
    return 0;
}
